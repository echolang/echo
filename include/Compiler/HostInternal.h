#ifndef HOSTINTERNAL_H
#define HOSTINTERNAL_H

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Compiler
{
    // **how a host argv[0] becomes a path.** HostProcess (wait) and HostCapture (pipes) both
    // spawn; one resolution so a bundled clang cannot be found by one and missed by the other
    std::string resolve_program(const std::string &name);

    bool is_unresolved_bare_name(const std::string &program);

#if !defined(_WIN32)
    // what a child is handed, **built before the fork**: its argv, and this process's environment
    // with `extra_env` added or replacing. setenv in the child allocates, and a child of a process
    // that had a thread running when it forked may not
    class ChildImage
    {
    public:
        ChildImage(
            std::vector<std::string> argv,
            const std::vector<std::pair<std::string, std::string>> &extra_env);
        ChildImage(const ChildImage &) = delete;
        ChildImage &operator=(const ChildImage &) = delete;

        char *const *argv() const {
            return _argv.data();
        }

        char *const *env() const {
            return _env.data();
        }

    private:
        std::vector<std::string> _argv_storage;
        std::vector<std::string> _env_storage;
        std::vector<char *> _argv;
        std::vector<char *> _env;
    };
#endif

#if defined(_WIN32)
    // UTF-8 / UTF-16 and CommandLineToArgvW quoting. wait (ExecuteAndWait env) and capture
    // (CreateProcess command line) both need the same encoding
    std::wstring widen(const std::string &text);
    std::string narrow(const wchar_t *text, size_t length);
    std::wstring quote_windows_arg_wide(const std::string &arg);
    bool env_names_match(const std::string &left, const std::string &right);
#endif
};

#endif
