#include "subprocess.h"

#include "Compiler/HostTool.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace EchoTests
{
    ProcessResult run_process(
        const std::vector<std::string> &argv,
        unsigned timeout_ms,
        const std::filesystem::path &working_directory,
        const std::vector<std::pair<std::string, std::string>> &extra_env,
        const std::string &stdin_content
    )
    {
        const Compiler::CapturedProcess captured = Compiler::run_captured(
            argv, timeout_ms, working_directory, extra_env, stdin_content);

        ProcessResult result;
        result.exit_code = captured.exit_code;
        result.output = captured.output;
        result.timed_out = captured.timed_out;
        return result;
    }

    ProcessResult run_capturing(const std::string &command, unsigned timeout_ms)
    {
        const Compiler::CapturedProcess captured = Compiler::run_shell(command, timeout_ms);

        ProcessResult result;
        result.exit_code = captured.exit_code;
        result.output = captured.output;
        result.timed_out = captured.timed_out;
        return result;
    }
};

TEST_CASE("run_capturing kills a hung command", "[subprocess]")
{
    const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
    const EchoTests::ProcessResult result = EchoTests::run_capturing("ping -n 30 127.0.0.1 >nul", 200);
#else
    const EchoTests::ProcessResult result = EchoTests::run_capturing("sleep 30", 200);
#endif
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    REQUIRE(result.timed_out);
    REQUIRE(elapsed < 5000);
}

TEST_CASE("run_capturing still reports a clean exit", "[subprocess]")
{
#if defined(_WIN32)
    const EchoTests::ProcessResult result = EchoTests::run_capturing("echo hi", 2000);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("hi") != std::string::npos);
#else
    const EchoTests::ProcessResult result = EchoTests::run_capturing("printf hi", 2000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output == "hi");
#endif
}

TEST_CASE("run_capturing kills a hung pipeline, not only the shell", "[subprocess]")
{
    // redirection and `;` / `&` stop the shell from exec'ing — the sleeper is a
    // grandchild. a kill of the shell alone would leave it running
    const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
    const EchoTests::ProcessResult result =
        EchoTests::run_capturing("ping -n 30 127.0.0.1 >nul & ping -n 30 127.0.0.1 >nul", 200);
#else
    const EchoTests::ProcessResult result = EchoTests::run_capturing("sleep 30; true", 200);
#endif
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    REQUIRE(result.timed_out);
    REQUIRE(elapsed < 5000);
}

#if defined(_WIN32)
TEST_CASE("quote_windows_arg follows CommandLineToArgvW", "[subprocess]")
{
    REQUIRE(Compiler::quote_windows_arg("plain") == "plain");
    REQUIRE(Compiler::quote_windows_arg("") == "\"\"");
    REQUIRE(Compiler::quote_windows_arg("foo bar") == "\"foo bar\"");
    REQUIRE(Compiler::quote_windows_arg("C:\\foo bar\\") == "\"C:\\foo bar\\\\\"");
    REQUIRE(Compiler::quote_windows_arg("say \"hi\"") == "\"say \\\"hi\\\"\"");
}
#endif

TEST_CASE("process_directory is the running executable's folder", "[subprocess]")
{
    const std::filesystem::path dir = Compiler::process_directory();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(std::filesystem::is_directory(dir));
}
