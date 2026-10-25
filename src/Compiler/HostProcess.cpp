#include "Compiler/HostTool.h"

#include "Compiler/ProgressReporter.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// POSIX declares it and no header has to: Darwin's unistd.h does not
extern char **environ;
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

        if (const std::optional<std::string> found = Compiler::find_program_on_path(name)) {
            return found.value();
        }

        return name;
    }

    bool is_unresolved_bare_name(const std::string &program)
    {
        return !std::filesystem::path(program).has_parent_path();
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

    bool env_names_match(const std::string &left, const std::string &right)
    {
        if (left.size() != right.size()) {
            return false;
        }

        for (size_t i = 0; i < left.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(left[i]))
                != std::tolower(static_cast<unsigned char>(right[i]))) {
                return false;
            }
        }

        return true;
    }

    std::wstring windows_environment(const std::vector<std::pair<std::string, std::string>> &extra_env)
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

            // Windows names are case-insensitive: `PATH` has to replace `Path`
            // a second copy in the block is ignored, and the child would keep the parent's
            for (const auto &pair : extra_env) {
                if (env_names_match(pair.first, key)) {
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

    // what a child is handed, **built before the fork**: its argv, and this process's environment with
    // `extra_env` added or replacing. setenv in the child allocates, and a child of a process that had a
    // thread running when it forked may not. Pinned in place, since the two `char *` arrays point into it
    class ChildImage
    {
    public:
        ChildImage(std::vector<std::string> argv, const std::vector<std::pair<std::string, std::string>> &extra_env) :
            _argv_storage(std::move(argv))
        {
            for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
                const std::string spelled(*entry);
                const std::string name = spelled.substr(0, spelled.find('='));

                const bool replaced = std::any_of(extra_env.begin(), extra_env.end(), [&name](const auto &pair) {
                    return pair.first == name;
                });

                if (!replaced) {
                    _env_storage.push_back(spelled);
                }
            }

            for (const auto &[name, value] : extra_env) {
                _env_storage.push_back(name + "=" + value);
            }

            for (std::string &arg : _argv_storage) {
                _argv.push_back(arg.data());
            }

            for (std::string &entry : _env_storage) {
                _env.push_back(entry.data());
            }

            _argv.push_back(nullptr);
            _env.push_back(nullptr);
        }

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
};

std::optional<std::string> Compiler::find_program_on_path(const std::string &name)
{
#if defined(_WIN32)
    if (name.find_first_of("/\\") != std::string::npos) {
        return name;
    }

    std::vector<std::string> dirs;

    if (const char *path = std::getenv("PATH"); path != nullptr) {
        std::string_view rest(path);

        while (!rest.empty()) {
            const size_t end = rest.find(';');
            std::string_view entry = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);

            if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
                entry = entry.substr(1, entry.size() - 2);
            }

            if (!entry.empty() && std::filesystem::path(entry).is_absolute()) {
                dirs.emplace_back(entry);
            }
        }
    }

    if (dirs.empty()) {
        return std::nullopt;
    }

    const std::vector<llvm::StringRef> paths(dirs.begin(), dirs.end());
    llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(name, paths);
#else
    llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(name);
#endif

    if (!program) {
        return std::nullopt;
    }

    return program.get();
}

std::string Compiler::host_clang()
{
    return resolve_program("clang");
}

int Compiler::run_wait(const std::string &program, const std::vector<std::string> &argv)
{
    return run_wait(program, argv, {});
}

int Compiler::run_wait(
    const std::string &program,
    const std::vector<std::string> &argv,
    const std::vector<std::pair<std::string, std::string>> &extra_env
)
{
    if (argv.empty()) {
        return 127;
    }

    std::error_code ec;

    if (is_unresolved_bare_name(program) || !std::filesystem::is_regular_file(program, ec)) {
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

    std::optional<llvm::ArrayRef<llvm::StringRef>> env;
    std::vector<llvm::StringRef> env_refs;
    std::vector<std::string> env_storage;
#if !defined(_WIN32)
    std::unique_ptr<ChildImage> image;
    if (!extra_env.empty()) {
        image = std::make_unique<ChildImage>(argv, extra_env);
        for (char *const *entry = image->env(); entry != nullptr && *entry != nullptr; ++entry) {
            env_refs.emplace_back(*entry);
        }
        env = env_refs;
    }
#else
    if (!extra_env.empty()) {
        const wchar_t *block = GetEnvironmentStringsW();
        if (block != nullptr) {
            const wchar_t *cursor = block;
            while (*cursor != L'\0') {
                const std::wstring entry(cursor);
                cursor += entry.size() + 1;
                const size_t eq = entry.find(L'=');
                if (eq == std::wstring::npos || eq == 0) {
                    continue;
                }
                const std::string key = narrow(entry.c_str(), eq);
                bool replaced = false;
                for (const auto &pair : extra_env) {
                    if (env_names_match(pair.first, key)) {
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    env_storage.push_back(narrow(entry.c_str(), entry.size()));
                }
            }
            FreeEnvironmentStringsW(const_cast<wchar_t *>(block));
        }
        for (const auto &pair : extra_env) {
            env_storage.push_back(pair.first + "=" + pair.second);
        }
        for (const std::string &entry : env_storage) {
            env_refs.emplace_back(entry);
        }
        env = env_refs;
    }
#endif

    std::string error;
    bool failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        program, args, env, {}, 0, 0, &error, &failed);

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

namespace
{

enum class ChildStdio
{
    t_capture,
    t_stream_to_stderr,
};

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

    const std::string program = resolve_program(argv.front());
    std::vector<std::string> resolved = argv;
    resolved.front() = program;
    const bool capture = stdio == ChildStdio::t_capture;

    if (is_unresolved_bare_name(program)) {
        result.exit_code = 127;
        result.output = "could not find '" + program + "' on PATH.\n";
        return result;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    HANDLE read_pipe = INVALID_HANDLE_VALUE;
    HANDLE write_pipe = INVALID_HANDLE_VALUE;
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;

    if (capture) {
        if (!CreatePipe(&read_pipe, &write_pipe, &inherit, 0)) {
            result.exit_code = 127;
            result.output = "could not open a pipe to capture the child's output.\n";
            return result;
        }

        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

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
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read != INVALID_HANDLE_VALUE
        ? stdin_read
        : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = capture ? write_pipe : GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdError = capture ? write_pipe : GetStdHandle(STD_ERROR_HANDLE);

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

    if (write_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(write_pipe);
    }

    if (stdin_read != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_read);
    }

    if (!created) {
        if (read_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(read_pipe);
        }

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

    if (capture) {
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

        result.output = std::move(collected);
        result.output.erase(std::remove(result.output.begin(), result.output.end(), '\r'), result.output.end());
        CloseHandle(read_pipe);
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
    int pipe_ends[2] = { -1, -1 };
    int stdin_pipe[2] = { -1, -1 };

    if (capture) {
        if (pipe(pipe_ends) != 0) {
            result.exit_code = 127;
            result.output = "could not open a pipe to capture the child's output.\n";
            return result;
        }

        if (!stdin_content.empty() && pipe(stdin_pipe) != 0) {
            close(pipe_ends[0]);
            close(pipe_ends[1]);
            result.exit_code = 127;
            result.output = "could not open a pipe to feed the child's input.\n";
            return result;
        }
    }

    const ChildImage image(resolved, extra_env);

    std::fflush(nullptr);
    const pid_t child = fork();

    if (child < 0) {
        if (pipe_ends[0] != -1) {
            close(pipe_ends[0]);
            close(pipe_ends[1]);
        }

        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }

        result.exit_code = 127;
        result.output = "could not fork.\n";
        return result;
    }

    if (child == 0) {
        if (capture) {
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

    if (capture) {
        result.output.erase(std::remove(result.output.begin(), result.output.end(), '\r'), result.output.end());
    }

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
