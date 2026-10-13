#include "Compiler/Install.h"

#include <fmt/core.h>

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

std::filesystem::path env_path(const char *name)
{
    if (const char *value = std::getenv(name); value != nullptr && *value != '\0') {
        return std::filesystem::path(value);
    }

    return {};
}

// weakly_canonical so a directory that does not exist yet still matches the layout's name
// `--dir ecobuild` on a first install has no CACHEDIR.TAG to ask
std::filesystem::path settled_directory(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::path out = std::filesystem::weakly_canonical(path, ec);

    if (ec) {
        out = std::filesystem::absolute(path, ec);
    }

    return out.lexically_normal();
}

bool same_directory(const std::filesystem::path &left, const std::filesystem::path &right)
{
    std::error_code ec;

    if (std::filesystem::equivalent(left, right, ec) && !ec) {
        return true;
    }

    return settled_directory(left) == settled_directory(right);
}

};

bool Compiler::resolve_install_dir(
    const std::filesystem::path &flag_directory,
    std::filesystem::path &out_directory,
    std::string &out_reason
)
{
    out_reason.clear();

    std::error_code ec;

    // flag, then environment, then default, same order `--build-dir` already takes
    // the invocation describes this run, the environment describes this machine
    std::filesystem::path stated = flag_directory;

    if (stated.empty()) {
        stated = env_path("ECHO_BIN_DIR");
    }

    if (!stated.empty()) {
        out_directory = std::filesystem::absolute(stated, ec);

        if (ec) {
            out_reason = fmt::format("'{}' cannot be made absolute: {}", stated.string(), ec.message());
            return false;
        }

        return true;
    }

    // Windows: next to the echoc installer, `%LOCALAPPDATA%\echo\bin`
    // POSIX: `$HOME/.echo/bin`
    // deliberately not the cache root (`~/.cache/echo`): a PATH directory inside a cache is a
    // `clean` waiting to happen
#if defined(_WIN32)
    if (const std::filesystem::path local = env_path("LOCALAPPDATA"); !local.empty()) {
        out_directory = local / "echo" / "bin";
        return true;
    }

    if (const std::filesystem::path profile = env_path("USERPROFILE"); !profile.empty()) {
        out_directory = profile / ".echo" / "bin";
        return true;
    }
#endif

    if (const std::filesystem::path home = env_path("HOME"); !home.empty()) {
        out_directory = home / ".echo" / "bin";
        return true;
    }

    out_reason = "there is no home directory to install into - HOME is not set. Name a directory with "
        "'--dir', or set ECHO_BIN_DIR.";
    return false;
}

std::string Compiler::install_dir_refusal(
    const std::filesystem::path &directory,
    const BuildLayout &layout,
    const std::vector<Parser::ModuleManifest> &manifests
)
{
    if (is_echo_build_directory(directory) || same_directory(directory, layout.scratch_dir())) {
        return fmt::format(
            "'{}' is a build directory, and 'install' puts programs on a PATH 'echoc clean' must not "
            "empty. Name a different directory with '--dir', or set ECHO_BIN_DIR.",
            directory.string());
    }

    for (const Parser::ModuleManifest &manifest : manifests) {
        if (same_directory(directory, layout.module_dir(manifest))) {
            return fmt::format(
                "'{}' is a build directory, and 'install' puts programs on a PATH 'echoc clean' must not "
                "empty. Name a different directory with '--dir', or set ECHO_BIN_DIR.",
                directory.string());
        }
    }

    return {};
}

bool Compiler::install_binary(
    const std::filesystem::path &built,
    const std::filesystem::path &directory,
    const std::filesystem::path &file_name,
    std::string &out_reason
)
{
    out_reason.clear();

    std::error_code ec;

    // created, never marked: `prepare_build_directory` would drop a CACHEDIR.TAG here
    // then `echoc clean` would think it may empty somebody's PATH
    std::filesystem::create_directories(directory, ec);

    if (ec || !std::filesystem::is_directory(directory, ec)) {
        out_reason = fmt::format(
            "'{}' could not be created as a directory{}",
            directory.string(), ec ? ": " + ec.message() : ".");
        return false;
    }

#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif

    const std::filesystem::path destination = directory / file_name;
    const std::filesystem::path staged =
        directory / fmt::format(".{}.install-{}", file_name.string(), pid);

    auto drop_staged = [&]() {
        std::error_code ignored;
        std::filesystem::remove(staged, ignored);
    };

    if (std::filesystem::is_directory(destination, ec) && !ec) {
        out_reason = fmt::format(
            "'{}' is a directory, and 'install' replaces a program.",
            destination.string());
        return false;
    }

    std::filesystem::copy_file(built, staged, std::filesystem::copy_options::overwrite_existing, ec);

    if (ec) {
        out_reason = fmt::format(
            "'{}' could not be copied into '{}': {}",
            built.string(), directory.string(), ec.message());
        return false;
    }

    // copy the linker's mode onto the staged file so it is as executable as the original
    // two error_codes, because copy_file is not required to keep the mode
    // a failed status would otherwise feed `perms::unknown` into permissions() and still report success
    std::error_code status_ec;
    const std::filesystem::file_status built_status = std::filesystem::status(built, status_ec);

    if (status_ec) {
        drop_staged();
        out_reason = fmt::format(
            "could not read the mode of '{}': {}",
            built.string(), status_ec.message());
        return false;
    }

    std::error_code perm_ec;
    std::filesystem::permissions(staged, built_status.permissions(), perm_ec);

    if (perm_ec) {
        drop_staged();
        out_reason = fmt::format(
            "could not copy the mode of '{}' onto '{}': {}",
            built.string(), staged.string(), perm_ec.message());
        return false;
    }

    std::filesystem::path aside;
    bool moved_aside = false;

#if defined(_WIN32)
    // Windows will not replace a running executable, but it will rename one
    // the old binary steps aside first, and a leftover `.old` from a previous install is dropped
    if (std::filesystem::exists(destination, ec) && !ec) {
        aside = destination;
        aside.replace_extension(".old" + destination.extension().string());

        std::filesystem::remove(aside, ec);
        std::filesystem::rename(destination, aside, ec);

        if (ec) {
            drop_staged();
            out_reason = fmt::format(
                "'{}' could not be replaced: {}",
                destination.string(), ec.message());
            return false;
        }

        moved_aside = true;
    }
#endif

    std::filesystem::rename(staged, destination, ec);

    if (ec) {
        std::error_code ignored;

        // put the aside destination back before dropping the staged copy
        // a failed replace must not leave the name they run missing
        if (moved_aside) {
            std::filesystem::rename(aside, destination, ignored);
        }

        drop_staged();
        out_reason = fmt::format("'{}' could not be replaced: {}", destination.string(), ec.message());
        return false;
    }

    if (moved_aside) {
        std::error_code ignored;
        std::filesystem::remove(aside, ignored);
    }

    return true;
}

bool Compiler::directory_is_on_path(const std::filesystem::path &directory)
{
    const char *path = std::getenv("PATH");

    if (path == nullptr) {
        return false;
    }

#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    const std::string entries = path;
    size_t start = 0;

    while (start <= entries.size()) {
        size_t end = entries.find(separator, start);

        if (end == std::string::npos) {
            end = entries.size();
        }

        const std::string entry = entries.substr(start, end - start);
        std::error_code ec;

        // equivalent() rather than a string compare, so a trailing slash or a symlink still counts
        if (!entry.empty() && std::filesystem::equivalent(entry, directory, ec) && !ec) {
            return true;
        }

        start = end + 1;
    }

    return false;
}
