#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "subprocess.h"
#include "Compiler/HostTool.h"

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

TEST_CASE("extra_env PATH replaces the parent's Path", "[subprocess]")
{
    const EchoTests::ProcessResult result = EchoTests::run_process(
        { "cmd.exe", "/c", "echo %PATH%" },
        EchoTests::k_default_timeout_ms,
        {},
        { { "PATH", "C:\\echo-install-sentinel" } });

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("echo-install-sentinel") != std::string::npos);
}
#endif

TEST_CASE("process_directory is the running executable's folder", "[subprocess]")
{
    const std::filesystem::path dir = Compiler::process_directory();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(std::filesystem::is_directory(dir));
}

TEST_CASE("process_executable is the running executable", "[subprocess]")
{
    const std::filesystem::path exe = Compiler::process_executable();
    REQUIRE_FALSE(exe.empty());
    REQUIRE(std::filesystem::is_regular_file(exe));
    REQUIRE(exe.parent_path() == Compiler::process_directory());
}

#if !defined(_WIN32)
TEST_CASE("run_streamed joins the child's stdout to stderr, in its own directory and environment", "[subprocess]")
{
    // the redirect is only observable from outside this process, so both streams are pointed at files for
    // the length of the call and put back after
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("echoc-run-streamed-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir / "cwd");

    const std::filesystem::path out_path = dir / "stdout.txt";
    const std::filesystem::path err_path = dir / "stderr.txt";

    std::fflush(nullptr);

    const int saved_out = dup(STDOUT_FILENO);
    const int saved_err = dup(STDERR_FILENO);

    FILE *out_file = std::fopen(out_path.c_str(), "w");
    FILE *err_file = std::fopen(err_path.c_str(), "w");
    REQUIRE(out_file != nullptr);
    REQUIRE(err_file != nullptr);

    dup2(fileno(out_file), STDOUT_FILENO);
    dup2(fileno(err_file), STDERR_FILENO);

    const int status = Compiler::run_streamed(
        { "/bin/sh", "-c", "echo WRITTEN_TO_STDOUT; echo WRITTEN_TO_STDERR >&2; pwd; echo \"$ECHO_SENTINEL\"; exit 3" },
        dir / "cwd",
        { { "ECHO_SENTINEL", "the-sentinel-value" } });

    std::fflush(nullptr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    std::fclose(out_file);
    std::fclose(err_file);

    const auto read = [](const std::filesystem::path &path) {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    const std::string written_out = read(out_path);
    const std::string written_err = read(err_path);
    std::filesystem::remove_all(dir);

    INFO("stdout: " << written_out);
    INFO("stderr: " << written_err);

    REQUIRE(status == 3);
    REQUIRE(written_out.empty());
    REQUIRE(written_err.find("WRITTEN_TO_STDOUT") != std::string::npos);
    REQUIRE(written_err.find("WRITTEN_TO_STDERR") != std::string::npos);
    REQUIRE(written_err.find("cwd") != std::string::npos);
    REQUIRE(written_err.find("the-sentinel-value") != std::string::npos);
}
#endif
