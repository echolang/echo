#include "Compiler/HostTool.h"
#include "Compiler/HostInternal.h"

#include "Compiler/ProgressReporter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{

#if !defined(_WIN32)
    // both fds at once: draining one then the other deadlocks when the unread side fills
    void drain_two_fds(
        int out_fd,
        int err_fd,
        unsigned timeout_ms,
        bool &timed_out,
        std::string &out_text,
        std::string &err_text)
    {
        timed_out = false;
        char buffer[4096];
        const auto started = std::chrono::steady_clock::now();

        while (out_fd >= 0 || err_fd >= 0) {
            int wait_ms = -1;

            if (timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();

                if (elapsed >= static_cast<long long>(timeout_ms)) {
                    timed_out = true;
                    return;
                }

                wait_ms = static_cast<int>(static_cast<long long>(timeout_ms) - elapsed);
            }

            pollfd pfds[2]{};
            nfds_t n = 0;
            int out_idx = -1;
            int err_idx = -1;

            if (out_fd >= 0) {
                pfds[n].fd = out_fd;
                pfds[n].events = POLLIN;
                out_idx = static_cast<int>(n);
                n += 1;
            }

            if (err_fd >= 0) {
                pfds[n].fd = err_fd;
                pfds[n].events = POLLIN;
                err_idx = static_cast<int>(n);
                n += 1;
            }

            const int ready = poll(pfds, n, wait_ms);

            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }

                return;
            }

            if (ready == 0) {
                timed_out = true;
                return;
            }

            auto take = [&](int idx, int &fd, std::string &into) {
                if (idx < 0) {
                    return;
                }

                if ((pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                    return;
                }

                const ssize_t read_bytes = read(fd, buffer, sizeof(buffer));

                if (read_bytes > 0) {
                    into.append(buffer, static_cast<size_t>(read_bytes));
                    return;
                }

                if (read_bytes < 0 && errno == EINTR) {
                    return;
                }

                fd = -1;
            };

            take(out_idx, out_fd, out_text);
            take(err_idx, err_fd, err_text);
        }
    }
#endif

#if defined(_WIN32)
    std::wstring windows_command_line(const std::vector<std::string> &argv)
    {
        // cmd's `/c` remainder is a command string, not an argv word. CommandLineToArgvW
        // quoting a payload that already contains quotes (`"C:\path\echoc.exe" --help`)
        // is the encoding cmd then misreads as a program named `\"C:\path\echoc.exe\"`
        if (argv.size() >= 3) {
            const std::wstring first = Compiler::quote_windows_arg_wide(argv[0]);
            if (argv[1] == "/c"
                && (first == L"cmd.exe" || first.ends_with(L"\\cmd.exe") || first.ends_with(L"/cmd.exe"))) {
                return first + L" /c " + Compiler::widen(argv[2]);
            }
        }

        std::wstring line;

        for (const std::string &arg : argv) {
            if (!line.empty()) {
                line += L' ';
            }

            line += Compiler::quote_windows_arg_wide(arg);
        }

        return line;
    }

    std::wstring windows_environment(
        const std::vector<std::pair<std::string, std::string>> &extra_env
    )
    {
        if (extra_env.empty()) {
            return std::wstring();
        }

        const wchar_t *block = GetEnvironmentStringsW();

        if (block == nullptr) {
            return std::wstring();
        }

        std::vector<std::wstring> entries;
        const wchar_t *cursor = block;

        while (*cursor != L'\0') {
            const std::wstring entry(cursor);
            cursor += entry.size() + 1;

            const size_t eq = entry.find(L'=');

            if (eq == std::wstring::npos || eq == 0) {
                continue;
            }

            bool replaced = false;
            const std::string key = Compiler::narrow(entry.c_str(), eq);

            // Windows names are case-insensitive: `PATH` has to replace `Path`
            // a second copy in the block is ignored, and the child would keep the parent's
            for (const auto &pair : extra_env) {
                if (Compiler::env_names_match(pair.first, key)) {
                    replaced = true;
                    break;
                }
            }

            if (!replaced) {
                entries.push_back(entry);
            }
        }

        FreeEnvironmentStringsW(const_cast<wchar_t *>(block));

        for (const auto &pair : extra_env) {
            entries.push_back(Compiler::widen(pair.first) + L'=' + Compiler::widen(pair.second));
        }

        std::wstring merged;

        for (const std::wstring &entry : entries) {
            merged += entry;
            merged += L'\0';
        }

        merged += L'\0';
        return merged;
    }
#endif


enum class ChildStdio
{
    t_capture,
    t_stream_to_stderr,
};

void strip_cr(std::string &text)
{
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
}

void finish_captured(Compiler::CapturedProcess &result)
{
    strip_cr(result.stdout_text);
    strip_cr(result.stderr_text);
    result.output = result.stdout_text + result.stderr_text;
}

Compiler::CapturedProcess run_child(
    const std::vector<std::string> &argv,
    unsigned timeout_ms,
    const std::filesystem::path &working_directory,
    const std::vector<std::pair<std::string, std::string>> &extra_env,
    const std::string &stdin_content,
    ChildStdio stdio)
{
    Compiler::CapturedProcess result;

    Compiler::ProgressReporter::instance().suspend();

    const std::string program = Compiler::resolve_program(argv.front());
    std::vector<std::string> resolved = argv;
    resolved.front() = program;
    const bool capture = stdio == ChildStdio::t_capture;

    if (Compiler::is_unresolved_bare_name(program)) {
        result.exit_code = 127;
        result.output = "could not find '" + program + "' on PATH.\n";
        return result;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_read = INVALID_HANDLE_VALUE;
    HANDLE stderr_write = INVALID_HANDLE_VALUE;
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;

    auto close_handle = [](HANDLE &handle) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    };

    if (capture) {
        if (!CreatePipe(&stdout_read, &stdout_write, &inherit, 0)
            || !CreatePipe(&stderr_read, &stderr_write, &inherit, 0)) {
            close_handle(stdout_read);
            close_handle(stdout_write);
            close_handle(stderr_read);
            close_handle(stderr_write);
            result.exit_code = 127;
            result.output = "could not open a pipe to capture the child's output.\n";
            return result;
        }

        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

        if (!stdin_content.empty() && !CreatePipe(&stdin_read, &stdin_write, &inherit, 0)) {
            close_handle(stdout_read);
            close_handle(stdout_write);
            close_handle(stderr_read);
            close_handle(stderr_write);
            result.exit_code = 127;
            result.output = "could not open a pipe to feed the child's input.\n";
            return result;
        }

        if (stdin_write != INVALID_HANDLE_VALUE) {
            SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read != INVALID_HANDLE_VALUE
        ? stdin_read
        : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = capture ? stdout_write : GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdError = capture ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

    std::wstring command = windows_command_line(resolved);
    std::wstring wprogram = std::filesystem::path(program).wstring();
    std::wstring directory = working_directory.empty() ? std::wstring() : working_directory.wstring();
    std::wstring environment = windows_environment(extra_env);

    HANDLE job = nullptr;

    if (capture) {
        job = CreateJobObjectW(nullptr, nullptr);

        if (job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (!SetInformationJobObject(
                    job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                CloseHandle(job);
                job = nullptr;
            }
        }
    }

    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;

    if (job != nullptr) {
        flags |= CREATE_SUSPENDED;
    }

    const BOOL created = CreateProcessW(
        wprogram.c_str(),
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        flags,
        environment.empty() ? nullptr : environment.data(),
        directory.empty() ? nullptr : directory.c_str(),
        &startup,
        &process);

    close_handle(stdout_write);
    close_handle(stderr_write);
    close_handle(stdin_read);

    if (!created) {
        close_handle(stdout_read);
        close_handle(stderr_read);
        close_handle(stdin_write);

        if (job != nullptr) {
            CloseHandle(job);
        }

        result.exit_code = 127;
        result.output = "could not start '" + program + "'.\n";
        return result;
    }

    if (job != nullptr) {
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }

        ResumeThread(process.hThread);
    }

    if (stdin_write != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(
            stdin_write,
            stdin_content.data(),
            static_cast<DWORD>(stdin_content.size()),
            &written,
            nullptr);
        close_handle(stdin_write);
    }

    if (capture) {
        const auto started = std::chrono::steady_clock::now();
        char buffer[4096];

        auto read_available = [&](HANDLE pipe, std::string &into) {
            DWORD available = 0;

            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
                return false;
            }

            DWORD got = 0;
            const DWORD want = available > sizeof(buffer) ? static_cast<DWORD>(sizeof(buffer)) : available;

            if (!ReadFile(pipe, buffer, want, &got, nullptr) || got == 0) {
                return false;
            }

            into.append(buffer, static_cast<size_t>(got));
            return true;
        };

        while (true) {
            while (read_available(stdout_read, result.stdout_text)
                || read_available(stderr_read, result.stderr_text)) {
            }

            const DWORD wait = WaitForSingleObject(process.hProcess, 15);

            if (wait == WAIT_OBJECT_0) {
                while (read_available(stdout_read, result.stdout_text)
                    || read_available(stderr_read, result.stderr_text)) {
                }

                break;
            }

            if (timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();

                if (elapsed >= static_cast<long long>(timeout_ms)) {
                    result.timed_out = true;

                    if (job != nullptr) {
                        TerminateJobObject(job, 1);
                    }
                    else {
                        TerminateProcess(process.hProcess, 1);
                    }

                    WaitForSingleObject(process.hProcess, INFINITE);
                    break;
                }
            }
        }

        finish_captured(result);
        close_handle(stdout_read);
        close_handle(stderr_read);
    }
    else {
        WaitForSingleObject(process.hProcess, INFINITE);
    }

    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);

    if (job != nullptr) {
        CloseHandle(job);
    }

    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    result.exit_code = result.timed_out ? 1 : static_cast<int>(code);
    return result;
#else
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    int stdin_pipe[2] = { -1, -1 };

    auto close_pipe = [](int fds[2]) {
        if (fds[0] != -1) {
            close(fds[0]);
            fds[0] = -1;
        }

        if (fds[1] != -1) {
            close(fds[1]);
            fds[1] = -1;
        }
    };

    if (capture) {
        if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
            close_pipe(stdout_pipe);
            close_pipe(stderr_pipe);
            result.exit_code = 127;
            result.output = "could not open a pipe to capture the child's output.\n";
            return result;
        }

        if (!stdin_content.empty() && pipe(stdin_pipe) != 0) {
            close_pipe(stdout_pipe);
            close_pipe(stderr_pipe);
            result.exit_code = 127;
            result.output = "could not open a pipe to feed the child's input.\n";
            return result;
        }
    }

    const Compiler::ChildImage image(resolved, extra_env);

    std::fflush(nullptr);
    const pid_t child = fork();

    if (child < 0) {
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        close_pipe(stdin_pipe);
        result.exit_code = 127;
        result.output = "could not fork.\n";
        return result;
    }

    if (child == 0) {
        if (capture) {
            setpgid(0, 0);
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);

            if (stdin_pipe[0] != -1) {
                dup2(stdin_pipe[0], STDIN_FILENO);
                close(stdin_pipe[0]);
                close(stdin_pipe[1]);
            }
        }
        else {
            dup2(STDERR_FILENO, STDOUT_FILENO);
        }

        if (!working_directory.empty()) {
            if (chdir(working_directory.string().c_str()) != 0) {
                _exit(127);
            }
        }

        execve(program.c_str(), image.argv(), image.env());
        _exit(127);
    }

    if (capture) {
        setpgid(child, child);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
            const ssize_t wrote = write(stdin_pipe[1], stdin_content.data(), stdin_content.size());
            (void)wrote;
            close(stdin_pipe[1]);
        }

        bool timed_out = false;
        drain_two_fds(
            stdout_pipe[0],
            stderr_pipe[0],
            timeout_ms,
            timed_out,
            result.stdout_text,
            result.stderr_text);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        finish_captured(result);

        if (timed_out) {
            if (kill(-child, SIGKILL) != 0) {
                kill(child, SIGKILL);
            }

            result.timed_out = true;
        }
    }

    int status = 0;

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            result.exit_code = 127;
            return result;
        }
    }

    if (result.timed_out) {
        result.exit_code = 128 + SIGKILL;
        result.signal = SIGKILL;
        return result;
    }

    if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
        result.exit_code = 128 + result.signal;
        return result;
    }

    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 127;
    return result;
#endif
}

};

Compiler::CapturedProcess Compiler::run_captured(
    const std::vector<std::string> &argv,
    unsigned timeout_ms,
    const std::filesystem::path &working_directory,
    const std::vector<std::pair<std::string, std::string>> &extra_env,
    const std::string &stdin_content
)
{
    if (argv.empty()) {
        CapturedProcess result;
        result.exit_code = 127;
        result.output = "run_captured: empty argv\n";
        return result;
    }

    return run_child(
        argv, timeout_ms, working_directory, extra_env, stdin_content, ChildStdio::t_capture);
}

int Compiler::run_streamed(
    const std::vector<std::string> &argv,
    const std::filesystem::path &working_directory,
    const std::vector<std::pair<std::string, std::string>> &extra_env
)
{
    if (argv.empty()) {
        return 127;
    }

    return run_child(
        argv, 0, working_directory, extra_env, {}, ChildStdio::t_stream_to_stderr).exit_code;
}

Compiler::CapturedProcess Compiler::run_shell(const std::string &command, unsigned timeout_ms)
{
#if defined(_WIN32)
    return run_captured({ "cmd.exe", "/c", command }, timeout_ms);
#else
    return run_captured({ "/bin/sh", "-c", command }, timeout_ms);
#endif
}
