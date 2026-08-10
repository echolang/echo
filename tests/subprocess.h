#ifndef TESTS_SUBPROCESS_H
#define TESTS_SUBPROCESS_H

#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
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

    // the one line of a report whose first whitespace-separated field is `first_field`, or "".
    //
    // both `--explain-cache` and `[clean]` print one whitespace-aligned row per module and both suites ask
    // the same question of one - which field follows is what they differ in, and that is the assertion
    inline std::string line_starting_with(const std::string &output, const std::string &first_field)
    {
        std::istringstream stream(output);
        std::string line;

        while (std::getline(stream, line)) {
            std::istringstream fields(line);
            std::string first;
            fields >> first;

            if (first == first_field) {
                return line;
            }
        }

        return "";
    }

    inline void write_file(const std::filesystem::path &path, const std::string &content)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    // a scratch project directory, removed when the test leaves. Named after the *suite* and then the
    // case, so a failure leaves something identifiable behind when the removal is commented out to
    // inspect it - and so two suites cannot collide on a case name
    class ScopedProject
    {
    public:
        ScopedProject(const std::string &suite, const std::string &name) :
            _root(std::filesystem::path(ECO_E2E_TMP_DIR) / suite / name)
        {
            std::error_code ec;
            std::filesystem::remove_all(_root, ec);
            std::filesystem::create_directories(_root, ec);
        }

        ~ScopedProject()
        {
            std::error_code ec;
            std::filesystem::remove_all(_root, ec);
        }

        ScopedProject(const ScopedProject &) = delete;
        ScopedProject &operator=(const ScopedProject &) = delete;

        const std::filesystem::path &root() const { return _root; }
        std::filesystem::path build_dir() const { return _root / "build"; }

        // `cd <dir> && echoc <args>`, because the working directory is what project discovery and
        // manifest resolution both read. The root is the default, which is what every case but the
        // ones testing discovery itself wants
        ProcessResult echoc(const std::string &args, const std::filesystem::path &working_directory) const
        {
            return run_capturing(
                "cd " + quoted(working_directory) + " && " + quoted(ECHOC_BINARY) + " " + args + " 2>&1");
        }

        ProcessResult echoc(const std::string &args) const { return echoc(args, _root); }

    private:
        std::filesystem::path _root;
    };
};

#endif
