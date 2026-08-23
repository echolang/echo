#include "Compiler/TestRunner.h"

#include "Compiler/HostTool.h"
#include "Compiler/ProgressReporter.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#define ECO_TEST_ISOLATION_POSIX 1
#else
#define ECO_TEST_ISOLATION_POSIX 0
#endif

#if ECO_TEST_ISOLATION_POSIX
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    // one pass/fail rule for both overloads, so the JIT fork and the linked runner cannot disagree.
    // a fired `--timeout` stays t_timed_out. a signal stays t_signalled: a segfault is not
    // `__eco_abort`, and v1 does not count it as an expected death. a non-zero *exit* is the
    // inverted pass when the test asked for death - die and a failed assert are both that exit
    Compiler::TestOutcome isolated_outcome(
        const Compiler::TestCase &test,
        bool timed_out,
        int signal,
        int status
    )
    {
        if (timed_out) {
            return Compiler::TestOutcome::t_timed_out;
        }

        if (signal != 0) {
            return Compiler::TestOutcome::t_signalled;
        }

        if (test.expects_death) {
            return status != 0
                ? Compiler::TestOutcome::t_passed
                : Compiler::TestOutcome::t_failed;
        }

        return status == 0
            ? Compiler::TestOutcome::t_passed
            : Compiler::TestOutcome::t_failed;
    }

#if ECO_TEST_ISOLATION_POSIX
    // everything the child wrote, read to end of file or until the deadline.
    //
    // **read to EOF before the wait, never after.** A pipe holds a page or so; a test that prints more than
    // that blocks in `write` while the parent blocks in `waitpid`, and neither ever moves again. Draining
    // first cannot deadlock: the write end is closed in the parent, so EOF arrives when the last child
    // holding it exits
    //
    // **poll + SIGKILL**, never `alarm()`. the flag is milliseconds and the parent already owns the
    // pipe; a deadline in the child is whole seconds and catchable. `timed_out` is a parent-side fact
    std::string drain(int fd, unsigned timeout_ms, bool &timed_out)
    {
        timed_out = false;
        std::string collected;
        char buffer[4096];
        const auto started = std::chrono::steady_clock::now();

        while (true) {
            int wait_ms = -1;

            if (timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();

                if (elapsed >= static_cast<long long>(timeout_ms)) {
                    timed_out = true;
                    return collected;
                }

                wait_ms = static_cast<int>(static_cast<long long>(timeout_ms) - elapsed);
            }

            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;

            const int ready = poll(&pfd, 1, wait_ms);

            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }

                return collected;
            }

            if (ready == 0) {
                timed_out = true;
                return collected;
            }

            const ssize_t read_bytes = read(fd, buffer, sizeof(buffer));

            if (read_bytes > 0) {
                collected.append(buffer, static_cast<size_t>(read_bytes));
                continue;
            }

            if (read_bytes < 0 && errno == EINTR) {
                continue;
            }

            return collected;
        }
    }

    // waitpid, retried past an interruption. anything else is the child gone in a way this cannot describe
    bool reap(pid_t child, int &out_status)
    {
        while (waitpid(child, &out_status, 0) < 0) {
            if (errno != EINTR) {
                return false;
            }
        }

        return true;
    }
#endif
};

Compiler::TestResult Compiler::run_test_isolated(
    const TestCase &test,
    const std::vector<std::string> &argv,
    unsigned timeout_ms
)
{
    TestResult result;
    result.test = test;

    const auto started = std::chrono::steady_clock::now();

    const CapturedProcess captured = run_captured(
        argv,
        timeout_ms,
        {},
        { { k_isolated_test_env, test.symbol } });

    result.milliseconds = progress_elapsed_ms(started);
    result.output = captured.output;
    result.status = captured.exit_code;
    result.signal = captured.signal;

    result.outcome = isolated_outcome(test, captured.timed_out, captured.signal, result.status);
    return result;
}

#if ECO_TEST_ISOLATION_POSIX

Compiler::TestResult Compiler::run_test_isolated(
    const TestCase &test,
    const std::function<void()> &call,
    unsigned timeout_ms
)
{
    TestResult result;
    result.test = test;

    const auto started = std::chrono::steady_clock::now();

    // the same clock every phase of the compiler is timed on - Compiler::progress_elapsed_ms owns the
    // rounding, so a test's number and a checklist row's are read off one spelling
    const auto elapsed = [&started]() {
        return progress_elapsed_ms(started);
    };

    // **the isolation itself failing is a failed test, not a failed compiler** - three moments word it,
    // and each one owes the timing stamp a reader of the summary would otherwise see as a zero. one
    // spelling, so a fourth cannot forget it. `why` is empty where the outcome speaks for itself:
    // reaping tells us nothing a message could add
    const auto failed = [&result, &elapsed](const char *why) {
        result.outcome = TestOutcome::t_failed;
        result.status = -1;
        result.milliseconds = elapsed();

        if (why != nullptr) {
            result.output = why;
        }

        return result;
    };

    int pipe_ends[2] = { -1, -1 };

    if (pipe(pipe_ends) != 0) {
        return failed("echoc could not open a pipe to capture this test's output.\n");
    }

    // **before the fork, and this is not a nicety.** Anything still buffered in this process is copied into
    // the child by fork() and written a second time when the child flushes - so the compiler's own output
    // would appear once per test
    std::fflush(nullptr);

    const pid_t child = fork();

    if (child < 0) {
        close(pipe_ends[0]);
        close(pipe_ends[1]);

        return failed("echoc could not fork a process to run this test in.\n");
    }

    if (child == 0) {
        // the child. both streams go down the pipe, so a `die`'s message on fd 2 and an `echo`'s on fd 1
        // arrive in the order the test wrote them and belong to this test alone
        close(pipe_ends[0]);
        dup2(pipe_ends[1], STDOUT_FILENO);
        dup2(pipe_ends[1], STDERR_FILENO);
        close(pipe_ends[1]);

        call();

        // **`_exit` and never `exit`.** An atexit handler registered in the parent - LLVM installs several -
        // would run here too, in a process that owns none of what they are about to tear down. The buffers
        // are flushed by hand first, which is the one thing `exit` would have done that is wanted
        std::fflush(nullptr);
        _exit(0);
    }

    close(pipe_ends[1]);

    bool timed_out = false;
    result.output = drain(pipe_ends[0], timeout_ms, timed_out);
    close(pipe_ends[0]);

    if (timed_out) {
        kill(child, SIGKILL);
    }

    int status = 0;

    // the drained output is already on the result and is kept: what the test managed to say before the
    // wait went wrong is the only evidence there is
    if (!reap(child, status)) {
        return failed(nullptr);
    }

    result.milliseconds = elapsed();

    if (timed_out) {
        result.signal = SIGKILL;
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    } else {
        result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    result.outcome = isolated_outcome(test, timed_out, result.signal, result.status);

    return result;
}

#endif
