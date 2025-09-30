#ifndef TESTS_SUBPROCESS_H
#define TESTS_SUBPROCESS_H

#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/wait.h>

// the suites' process primitive. Two of them - the e2e corpus and the module cache - drive real `echoc`
// subprocesses, and they had a `popen` block each: same buffer, same wait-status decoding, same quoting.
// One place for it, so a future `timeout:` setting has one place to go and so the wait status is decoded
// exactly once.
namespace EchoTests
{
    // the exit status matters as much as the text: `expect` is what makes a deliberately broken program's
    // rejection part of the contract rather than a coincidence of what it happened to print
    struct ProcessResult
    {
        int exit_code = 0;
        std::string output;
    };

    // runs a shell command, capturing merged stdout+stderr. A signal is reported as `128 + signo`, the way a
    // shell reports it, which keeps a JIT segfault distinguishable from a clean rejection in the failure
    // message.
    //
    // a failure to spawn is reported through `exit_code` like any other, never with a Catch2 assertion: a
    // primitive that asserts can only be called from inside an assertion context, which a cached probe run
    // at static-init time is not
    inline ProcessResult run_capturing(const std::string &command)
    {
        ProcessResult result;

        std::array<char, 4096> buf;
        FILE *pipe = popen(command.c_str(), "r");

        if (!pipe) {
            result.exit_code = 127;
            result.output = "could not spawn: " + command;
            return result;
        }

        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
            result.output.append(buf.data(), n);
        }

        const int status = pclose(pipe);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

        return result;
    }

    // a path as one shell word. The corpus lives under a configured absolute directory, so a space in it is
    // somebody else's checkout rather than a hypothetical
    inline std::string quoted(const std::filesystem::path &path)
    {
        return "\"" + path.string() + "\"";
    }
};

#endif
