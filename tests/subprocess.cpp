#include "subprocess.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <string>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace EchoTests
{
    ProcessResult run_capturing(const std::string &command, unsigned timeout_ms)
    {
        ProcessResult result;

        int pipe_ends[2] = { -1, -1 };

        if (pipe(pipe_ends) != 0) {
            result.exit_code = 127;
            result.output = "could not open a pipe: " + command;
            return result;
        }

        const pid_t child = fork();

        if (child < 0) {
            close(pipe_ends[0]);
            close(pipe_ends[1]);
            result.exit_code = 127;
            result.output = "could not fork: " + command;
            return result;
        }

        if (child == 0) {
            setpgid(0, 0);
            close(pipe_ends[0]);
            dup2(pipe_ends[1], STDOUT_FILENO);
            dup2(pipe_ends[1], STDERR_FILENO);
            close(pipe_ends[1]);
            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        // both sides, so a kill before the child reaches setpgid still names the group
        setpgid(child, child);
        close(pipe_ends[1]);

        const auto started = std::chrono::steady_clock::now();
        std::array<char, 4096> buf{};

        while (true) {
            int wait_ms = -1;

            if (timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();

                if (elapsed >= static_cast<long long>(timeout_ms)) {
                    result.timed_out = true;
                    break;
                }

                wait_ms = static_cast<int>(static_cast<long long>(timeout_ms) - elapsed);
            }

            pollfd pfd{};
            pfd.fd = pipe_ends[0];
            pfd.events = POLLIN;

            const int ready = poll(&pfd, 1, wait_ms);

            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }

                break;
            }

            if (ready == 0) {
                result.timed_out = true;
                break;
            }

            if ((pfd.revents & POLLIN) != 0) {
                const ssize_t got = read(pipe_ends[0], buf.data(), buf.size());

                if (got > 0) {
                    result.output.append(buf.data(), static_cast<size_t>(got));
                    continue;
                }

                if (got == 0) {
                    break;
                }

                if (errno == EINTR) {
                    continue;
                }

                break;
            }

            if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                const ssize_t got = read(pipe_ends[0], buf.data(), buf.size());

                if (got > 0) {
                    result.output.append(buf.data(), static_cast<size_t>(got));
                    continue;
                }

                break;
            }
        }

        if (result.timed_out) {
            kill(-child, SIGKILL);
        }

        close(pipe_ends[0]);

        int status = 0;

        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                result.exit_code = 127;
                return result;
            }
        }

        if (result.timed_out) {
            result.exit_code = 128 + SIGKILL;
            const std::string notice = "timed out after " + std::to_string(timeout_ms) + " ms";
            result.output = result.output.empty() ? notice : result.output + "\n" + notice;
            return result;
        }

        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return result;
    }
};

TEST_CASE("run_capturing kills a hung command", "[subprocess]")
{
    const auto started = std::chrono::steady_clock::now();
    const EchoTests::ProcessResult result = EchoTests::run_capturing("sleep 30", 200);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    REQUIRE(result.timed_out);
    REQUIRE(result.exit_code == 128 + SIGKILL);
    REQUIRE(result.output.find("timed out after 200 ms") != std::string::npos);
    REQUIRE(elapsed < 5000);
}

TEST_CASE("run_capturing still reports a clean exit", "[subprocess]")
{
    const EchoTests::ProcessResult result = EchoTests::run_capturing("printf hi", 2000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output == "hi");
}

TEST_CASE("run_capturing kills a hung pipeline, not only the shell", "[subprocess]")
{
    // `2>&1` (and `;`) stop sh from exec'ing — the sleep is a grandchild. a
    // kill of the shell alone would leave it running
    const auto started = std::chrono::steady_clock::now();
    const EchoTests::ProcessResult result = EchoTests::run_capturing("sleep 30; true", 200);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    REQUIRE(result.timed_out);
    REQUIRE(result.exit_code == 128 + SIGKILL);
    REQUIRE(elapsed < 5000);
}
