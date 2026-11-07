#include "Compiler/HostTool.h"
#include "Compiler/HostInternal.h"

#include "Compiler/ProgressReporter.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
};

#if defined(_WIN32)
std::wstring Compiler::widen(const std::string &text)
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

std::string Compiler::narrow(const wchar_t *text, size_t length)
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
std::wstring Compiler::quote_windows_arg_wide(const std::string &arg)
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

bool Compiler::env_names_match(const std::string &left, const std::string &right)
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
#endif

std::string Compiler::resolve_program(const std::string &name)
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

bool Compiler::is_unresolved_bare_name(const std::string &program)
{
    return !std::filesystem::path(program).has_parent_path();
}

#if !defined(_WIN32)
Compiler::ChildImage::ChildImage(
    std::vector<std::string> argv,
    const std::vector<std::pair<std::string, std::string>> &extra_env
) :
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
#endif

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
