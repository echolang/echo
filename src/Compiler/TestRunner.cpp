#include "Compiler/TestRunner.h"

#include "Compiler/ProgressReporter.h"

#include <cerrno>
#include <chrono>
#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#define ECO_TEST_ISOLATION_POSIX 1
#else
#define ECO_TEST_ISOLATION_POSIX 0
#endif

#if ECO_TEST_ISOLATION_POSIX
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
#if ECO_TEST_ISOLATION_POSIX
    // everything the child wrote, read to end of file.
    //
    // **read to EOF before the wait, never after.** A pipe holds a page or so; a test that prints more than
    // that blocks in `write` while the parent blocks in `waitpid`, and neither ever moves again. Draining
    // first cannot deadlock: the write end is closed in the parent, so EOF arrives when the last child
    // holding it exits
    std::string drain(int fd)
    {
        std::string collected;
        char buffer[4096];

        while (true) {
            const ssize_t read_bytes = read(fd, buffer, sizeof(buffer));

            if (read_bytes > 0) {
                collected.append(buffer, static_cast<size_t>(read_bytes));
                continue;
            }

            // a signal interrupted the read rather than ending it - the child is still going
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

bool Compiler::test_isolation_available()
{
    return ECO_TEST_ISOLATION_POSIX != 0;
}

#if ECO_TEST_ISOLATION_POSIX

Compiler::TestResult Compiler::run_test_isolated(
    const TestCase &test,
    const std::function<void()> &call
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

    int pipe_ends[2] = { -1, -1 };

    if (pipe(pipe_ends) != 0) {
        result.outcome = TestOutcome::t_failed;
        result.status = -1;
        result.output = "echoc could not open a pipe to capture this test's output.\n";
        result.milliseconds = elapsed();

        return result;
    }

    // **before the fork, and this is not a nicety.** Anything still buffered in this process is copied into
    // the child by fork() and written a second time when the child flushes - so the compiler's own output
    // would appear once per test
    std::fflush(nullptr);

    const pid_t child = fork();

    if (child < 0) {
        close(pipe_ends[0]);
        close(pipe_ends[1]);

        result.outcome = TestOutcome::t_failed;
        result.status = -1;
        result.output = "echoc could not fork a process to run this test in.\n";
        result.milliseconds = elapsed();

        return result;
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

    result.output = drain(pipe_ends[0]);
    close(pipe_ends[0]);

    int status = 0;

    if (!reap(child, status)) {
        result.outcome = TestOutcome::t_failed;
        result.status = -1;
        result.milliseconds = elapsed();

        return result;
    }

    result.milliseconds = elapsed();

    if (WIFSIGNALED(status)) {
        result.outcome = TestOutcome::t_signalled;
        result.signal = WTERMSIG(status);

        return result;
    }

    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    // **any non-zero exit is a failure and echoc does not care which.** A failed `assert` and a `die` both
    // reach the abort thunk's `exit(1)` having already written their message, and a test calling
    // `std::env::exit(3)` has ended itself without saying it passed
    result.outcome = result.status == 0 ? TestOutcome::t_passed : TestOutcome::t_failed;

    return result;
}

#else

Compiler::TestResult Compiler::run_test_isolated(
    const TestCase &test,
    const std::function<void()> &call
)
{
    (void)call;

    // unreachable: the subcommand refuses before the first test when test_isolation_available() is false,
    // which is where the refusal belongs - a platform that cannot isolate is told so once
    TestResult result;
    result.test = test;
    result.outcome = TestOutcome::t_failed;
    result.output = "running a test in its own process is not implemented on this platform.\n";

    return result;
}

#endif
