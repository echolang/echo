#include "Compiler/HostTool.h"

#include "Compiler/ProgressReporter.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
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
    std::string sibling_tool(const std::filesystem::path &clang, const std::string &name)
    {
        std::filesystem::path candidate = clang.parent_path() / name;

#if defined(_WIN32)
        if (candidate.extension().empty()) {
            candidate += ".exe";
        }
#endif

        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.string();
        }

        return {};
    }

    std::string resolve_program(const std::string &name)
    {
        // a released Windows echoc ships clang and lld-link next to itself.
        // look there before the baked build-machine path, which does not
        // exist on a machine that only ran the installer
        const std::string bundled = sibling_tool(Compiler::process_directory(), name);
        if (!bundled.empty()) {
            return bundled;
        }

        // prefer the LLVM that built echoc. findProgramByName walks PATH, and
        // llvm-mingw's GNU-ABI clang is a common first hit on Windows
#ifdef ECO_HOST_CLANG
        const std::filesystem::path baked(ECO_HOST_CLANG);
        std::error_code ec;

        if (name == "clang" || name == "clang.exe") {
            if (std::filesystem::is_regular_file(baked, ec)) {
                return baked.string();
            }
        }
        else {
            const std::string sibling = sibling_tool(baked, name);
            if (!sibling.empty()) {
                return sibling;
            }
        }
#endif

        llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(name);

        if (!program) {
            return name;
        }

        return program.get();
    }

#if defined(_WIN32)
    std::wstring widen(const std::string &text)
    {
        if (text.empty()) {
            return std::wstring();
        }

        const int needed = MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);

        if (needed <= 0) {
            return std::wstring();
        }

        std::wstring wide(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
        return wide;
    }

    std::string narrow(const wchar_t *text, size_t length)
    {
        if (text == nullptr || length == 0) {
            return std::string();
        }

        const int needed = WideCharToMultiByte(
            CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);

        if (needed <= 0) {
            return std::string();
        }

        std::string utf8(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, text, static_cast<int>(length), utf8.data(), needed, nullptr, nullptr);
        return utf8;
    }

    // CommandLineToArgvW: n backslashes before a non-quote stay n; n before a quote
    // become 2n+1 and the quote is literal; n at the end of a quoted word become 2n
    // so the closing delimiter is not eaten. empty words are `""`
    std::wstring quote_windows_arg_wide(const std::string &arg)
    {
        const std::wstring wide = widen(arg);
        const bool needs_quotes =
            wide.empty() || wide.find_first_of(L" \t\"") != std::wstring::npos;

        if (!needs_quotes) {
            return wide;
        }

        std::wstring quoted;
        quoted.push_back(L'"');
        size_t backslashes = 0;

        for (wchar_t c : wide) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }

            if (c == L'"') {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'"');
                backslashes = 0;
                continue;
            }

            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(c);
        }

        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    std::wstring windows_command_line(const std::vector<std::string> &argv)
    {
        // cmd's `/c` remainder is a command string, not an argv word. CommandLineToArgvW
        // quoting a payload that already contains quotes (`"C:\path\echoc.exe" --help`)
        // is the encoding cmd then misreads as a program named `\"C:\path\echoc.exe\"`
        if (argv.size() >= 3) {
            const std::wstring first = quote_windows_arg_wide(argv[0]);
            if (argv[1] == "/c"
                && (first == L"cmd.exe" || first.ends_with(L"\\cmd.exe") || first.ends_with(L"/cmd.exe"))) {
                return first + L" /c " + widen(argv[2]);
            }
        }

        std::wstring line;

        for (const std::string &arg : argv) {
            if (!line.empty()) {
                line += L' ';
            }

            line += quote_windows_arg_wide(arg);
        }

        return line;
    }

    std::wstring windows_environment(
        const std::vector<std::pair<std::string, std::string>> &extra_env)
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
            const std::string key = narrow(entry.c_str(), eq);

            for (const auto &pair : extra_env) {
                if (pair.first == key) {
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
            entries.push_back(widen(pair.first) + L'=' + widen(pair.second));
        }

        std::wstring merged;

        for (const std::wstring &entry : entries) {
            merged += entry;
            merged += L'\0';
        }

        merged += L'\0';
        return merged;
    }
#else
    std::string drain_fd(int fd, unsigned timeout_ms, bool &timed_out)
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
#endif
};

std::string Compiler::host_clang()
{
    return resolve_program("clang");
}

std::filesystem::path Compiler::process_directory()
{
    static const std::filesystem::path dir = [] {
#if defined(_WIN32)
        std::wstring buf(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0 || n >= buf.size()) {
            return std::filesystem::current_path();
        }
        buf.resize(n);
        return std::filesystem::path(buf).parent_path();
#else
        const std::string exe = llvm::sys::fs::getMainExecutable(
            "echoc", reinterpret_cast<void *>(&Compiler::process_directory));
        if (exe.empty()) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exe).parent_path();
#endif
    }();

    return dir;
}

std::filesystem::path Compiler::windows_sysroot()
{
#if !defined(_WIN32)
    return {};
#else
    static const std::filesystem::path root = [] {
        std::error_code ec;
        const std::filesystem::path dir = Compiler::process_directory();
        const std::filesystem::path candidates[] = {
            dir / "sysroot",
            dir.parent_path() / "sysroot",
        };

        for (const std::filesystem::path &candidate : candidates) {
            if (std::filesystem::is_directory(candidate / "lib", ec)) {
                return candidate;
            }
        }

        return std::filesystem::path();
    }();

    return root;
#endif
}

void Compiler::append_windows_sysroot_cc_args(std::vector<std::string> &argv)
{
#if !defined(_WIN32)
    (void)argv;
#else
    const std::filesystem::path sysroot = windows_sysroot();
    if (sysroot.empty()) {
        return;
    }

    const std::filesystem::path include = sysroot / "include";
    std::error_code ec;
    if (!std::filesystem::is_directory(include, ec)) {
        return;
    }

    std::vector<std::filesystem::path> dirs = { include };
    for (const auto &entry : std::filesystem::directory_iterator(include, ec)) {
        if (entry.is_directory(ec)) {
            dirs.push_back(entry.path());
        }
    }

    std::sort(dirs.begin(), dirs.end());
    for (const std::filesystem::path &dir : dirs) {
        argv.push_back("-isystem");
        argv.push_back(dir.string());
    }
#endif
}

void Compiler::append_windows_sysroot_link_args(std::vector<std::string> &argv)
{
#if !defined(_WIN32)
    (void)argv;
#else
    argv.push_back("-fuse-ld=lld");
    const std::filesystem::path sysroot = windows_sysroot();
    if (sysroot.empty()) {
        return;
    }
    argv.push_back("-L" + (sysroot / "lib").string());
#endif
}

int Compiler::run_wait(const std::string &program, const std::vector<std::string> &argv)
{
    if (argv.empty()) {
        return 127;
    }

    std::error_code ec;

    if (!std::filesystem::is_regular_file(program, ec)) {
        return 127;
    }

    std::vector<llvm::StringRef> args(argv.begin(), argv.end());

    // **the obligation that comes with inheriting the streams.** The child is about to write into the
    // same stderr a progress row may be sitting on, and there is no lock either side can take because the
    // child is another process. Discharged here rather than at each call site for the reason the header
    // states the inheritance: it is this function's fact, so it is this function's consequence - and one
    // line here covers clang, ld, dsymutil and whatever is added next.
    //
    // sticky, so there is no matching call afterwards to forget: the next row this compile draws restores
    // itself. That is also what leaves the `llvm::errs()` notes right after a failed link needing no site
    // of their own
    ProgressReporter::instance().suspend();

    std::string error;
    bool failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        program, args, std::nullopt, {}, 0, 0, &error, &failed);

    if (failed) {
        llvm::errs() << "could not start '" << program << "'";
        if (!error.empty()) {
            llvm::errs() << ": " << error;
        }
        llvm::errs() << '\n';
        return 127;
    }

    return status;
}

int Compiler::run_wait(const std::vector<std::string> &argv)
{
    if (argv.empty()) {
        return 127;
    }

    std::vector<std::string> resolved = argv;
    resolved.front() = resolve_program(argv.front());
    return run_wait(resolved.front(), resolved);
}

bool Compiler::run_tool(const std::vector<std::string> &argv)
{
    return run_wait(argv) == 0;
}

#if defined(_WIN32)
std::string Compiler::quote_windows_arg(const std::string &arg)
{
    const std::wstring quoted = quote_windows_arg_wide(arg);
    return narrow(quoted.c_str(), quoted.size());
}
#endif

Compiler::CapturedProcess Compiler::run_captured(
    const std::vector<std::string> &argv,
    unsigned timeout_ms,
    const std::filesystem::path &working_directory,
    const std::vector<std::pair<std::string, std::string>> &extra_env,
    const std::string &stdin_content)
{
    CapturedProcess result;

    if (argv.empty()) {
        result.exit_code = 127;
        result.output = "run_captured: empty argv\n";
        return result;
    }

    ProgressReporter::instance().suspend();

    const std::string program = resolve_program(argv.front());
    std::vector<std::string> resolved = argv;
    resolved.front() = program;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    HANDLE read_pipe = INVALID_HANDLE_VALUE;
    HANDLE write_pipe = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&read_pipe, &write_pipe, &inherit, 0)) {
        result.exit_code = 127;
        result.output = "could not open a pipe to capture the child's output.\n";
        return result;
    }

    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;

    if (!stdin_content.empty() && !CreatePipe(&stdin_read, &stdin_write, &inherit, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result.exit_code = 127;
        result.output = "could not open a pipe to feed the child's input.\n";
        return result;
    }

    if (stdin_write != INVALID_HANDLE_VALUE) {
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read != INVALID_HANDLE_VALUE
        ? stdin_read
        : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;

    std::wstring command = windows_command_line(resolved);
    std::wstring wprogram = std::filesystem::path(program).wstring();
    std::wstring directory = working_directory.empty() ? std::wstring() : working_directory.wstring();
    std::wstring environment = windows_environment(extra_env);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);

    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        if (!SetInformationJobObject(
                job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
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

    CloseHandle(write_pipe);

    if (stdin_read != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_read);
    }

    if (!created) {
        CloseHandle(read_pipe);

        if (stdin_write != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_write);
        }

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
        CloseHandle(stdin_write);
    }

    const auto started = std::chrono::steady_clock::now();
    std::string collected;
    char buffer[4096];

    while (true) {
        DWORD available = 0;

        if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            const DWORD want = available > sizeof(buffer) ? static_cast<DWORD>(sizeof(buffer)) : available;

            if (ReadFile(read_pipe, buffer, want, &got, nullptr) && got > 0) {
                collected.append(buffer, static_cast<size_t>(got));
                continue;
            }
        }

        const DWORD wait = WaitForSingleObject(process.hProcess, 15);

        if (wait == WAIT_OBJECT_0) {
            DWORD leftover = 0;

            while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &leftover, nullptr) && leftover > 0) {
                DWORD got = 0;
                const DWORD want = leftover > sizeof(buffer) ? static_cast<DWORD>(sizeof(buffer)) : leftover;

                if (!ReadFile(read_pipe, buffer, want, &got, nullptr) || got == 0) {
                    break;
                }

                collected.append(buffer, static_cast<size_t>(got));
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

    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);

    if (job != nullptr) {
        CloseHandle(job);
    }

    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(read_pipe);

    result.output = std::move(collected);
    result.output.erase(std::remove(result.output.begin(), result.output.end(), '\r'), result.output.end());
    result.exit_code = result.timed_out ? 1 : static_cast<int>(code);
    return result;
#else
    int pipe_ends[2] = { -1, -1 };

    if (pipe(pipe_ends) != 0) {
        result.exit_code = 127;
        result.output = "could not open a pipe to capture the child's output.\n";
        return result;
    }

    int stdin_pipe[2] = { -1, -1 };

    if (!stdin_content.empty() && pipe(stdin_pipe) != 0) {
        close(pipe_ends[0]);
        close(pipe_ends[1]);
        result.exit_code = 127;
        result.output = "could not open a pipe to feed the child's input.\n";
        return result;
    }

    std::fflush(nullptr);
    const pid_t child = fork();

    if (child < 0) {
        close(pipe_ends[0]);
        close(pipe_ends[1]);

        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }

        result.exit_code = 127;
        result.output = "could not fork.\n";
        return result;
    }

    if (child == 0) {
        setpgid(0, 0);
        close(pipe_ends[0]);
        dup2(pipe_ends[1], STDOUT_FILENO);
        dup2(pipe_ends[1], STDERR_FILENO);
        close(pipe_ends[1]);

        if (stdin_pipe[0] != -1) {
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }

        if (!working_directory.empty()) {
            if (chdir(working_directory.string().c_str()) != 0) {
                _exit(127);
            }
        }

        for (const auto &pair : extra_env) {
            setenv(pair.first.c_str(), pair.second.c_str(), 1);
        }

        std::vector<char *> child_argv;
        child_argv.reserve(resolved.size() + 1);

        for (std::string &arg : resolved) {
            child_argv.push_back(arg.data());
        }

        child_argv.push_back(nullptr);
        execv(program.c_str(), child_argv.data());
        _exit(127);
    }

    setpgid(child, child);
    close(pipe_ends[1]);

    if (stdin_pipe[0] != -1) {
        close(stdin_pipe[0]);
        const ssize_t wrote = write(stdin_pipe[1], stdin_content.data(), stdin_content.size());
        (void)wrote;
        close(stdin_pipe[1]);
    }

    bool timed_out = false;
    result.output = drain_fd(pipe_ends[0], timeout_ms, timed_out);
    close(pipe_ends[0]);

    if (timed_out) {
        if (kill(-child, SIGKILL) != 0) {
            kill(child, SIGKILL);
        }

        result.timed_out = true;
    }

    int status = 0;

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            result.exit_code = 127;
            return result;
        }
    }

    if (timed_out) {
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
    result.output.erase(std::remove(result.output.begin(), result.output.end(), '\r'), result.output.end());
    return result;
#endif
}

Compiler::CapturedProcess Compiler::run_shell(const std::string &command, unsigned timeout_ms)
{
#if defined(_WIN32)
    return run_captured({ "cmd.exe", "/c", command }, timeout_ms);
#else
    return run_captured({ "/bin/sh", "-c", command }, timeout_ms);
#endif
}
