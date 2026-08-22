#ifndef HOSTTOOL_H
#define HOSTTOOL_H

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Compiler
{
    // **the sole way echoc runs another program**, and the whole of what one costs a caller.
    //
    // **as an argv, with no shell in between.** An SDK path, an output name or a vendored source directory
    // may contain a space, and a quoting layer over std::system has no answer at all for one containing a
    // quote - which is what this replaced. A new tool's arguments go in as words, never as a command line.
    //
    // true when the program was found and exited zero. Its own stdout and stderr are inherited, so a
    // clang diagnostic reaches the terminal as clang wrote it rather than through a renderer that would
    // have to pretend it understood it
    bool run_tool(const std::vector<std::string> &argv);

    // the same spawn as run_tool, returning the child's exit code. 127 if the program was not found
    // or would not start. `echoc run` on a host that cannot JIT uses this so the program's status is
    // the process's status
    int run_wait(const std::vector<std::string> &argv);

    // execute `program` with `argv` as the child's argv. argv[0] is the name the child
    // sees, which is not always the path that was exec'd - a JIT-compatible `echoc run`
    // keeps the source file as argv[0] while spawning the scratch binary
    int run_wait(const std::string &program, const std::vector<std::string> &argv);

#if defined(_WIN32)
    // CommandLineToArgvW encoding of one argv word: 2n / 2n+1 backslashes before a quote, empty
    // arguments as `""`. CreateProcess is the consumer; tests pin the encoding
    std::string quote_windows_arg(const std::string &arg);
#endif

    // the clang this echoc was built with, so a GNU-ABI `clang` sitting first
    // on PATH (llvm-mingw) cannot link the MSVC objects we emit. empty if none
    std::string host_clang();

    // the same spawn, capturing merged stdout+stderr instead of inheriting them.
    //
    // a test's output belongs under its own row, and a corpus case's golden is a byte comparison, so
    // neither can inherit the streams the way clang's diagnostics do. `timeout_ms` of 0 waits forever.
    // `working_directory` empty means the caller's. `extra_env` is added to (or replaces) the child's
    // copy of this process's environment, so a parallel spawn cannot leak a variable into the parent.
    // `stdin_content` is written to the child's standard input and then closed, so a `readline` case
    // sees end-of-file after the last line; empty inherits the caller's stdin.
    //
    // a fired deadline kills the process group (POSIX) or the job object (Windows), so `echoc build`
    // cannot leave clang running after the parent is gone
    struct CapturedProcess
    {
        int exit_code = 0;
        std::string output;
        bool timed_out = false;

        // POSIX signal number, or 0. Windows has no signals; a crash is a non-zero exit_code
        int signal = 0;
    };

    CapturedProcess run_captured(
        const std::vector<std::string> &argv,
        unsigned timeout_ms = 0,
        const std::filesystem::path &working_directory = {},
        const std::vector<std::pair<std::string, std::string>> &extra_env = {},
        const std::string &stdin_content = {});

    // a shell command, for tests that already spelled `2>&1` / `2>nul`. Windows is `cmd /c`
    // with the payload unquoted as a unit; POSIX is `sh -c`
    CapturedProcess run_shell(const std::string &command, unsigned timeout_ms = 0);
};

#endif
