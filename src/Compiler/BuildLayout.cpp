#include "Compiler/BuildLayout.h"

#include "eco.h"

#include <fmt/core.h>

#include <cstdlib>
#include <fstream>

namespace
{

// the Cache Directory Tagging Specification's signature, which has to be the first 43 bytes of the file for
// tar --exclude-caches, restic, borg and Time Machine to skip the directory holding it. Copying a build
// directory into a backup is nobody's intent, and getting that for free is worth writing the line
constexpr std::string_view k_cachedir_signature = "Signature: 8a477f597d28d172789f06886806bc55";

// **ours as well as a cache's.** The signature alone says "some tool's cache", and a `--build-dir` pointed
// at another tool's would then be a directory `echoc clean` believes it may delete. Both lines have to be
// there before anything is removed
constexpr std::string_view k_echo_marker = "# echoc-build-directory";

std::string marker_contents()
{
    return fmt::format(
        "{}\n"
        "{}\n"
        "# Compiled artifacts for an Echo module. Written by echoc {}.\n"
        "# 'echoc clean' removes this directory; nothing in it is source.\n",
        k_cachedir_signature, k_echo_marker, ECO_VERSION_STRING);
}

std::filesystem::path marker_path(const std::filesystem::path &directory)
{
    return directory / "CACHEDIR.TAG";
}

// `$XDG_CACHE_HOME/echo`, else `$HOME/.cache/echo`, else empty when neither is set.
// Windows has no HOME by default: `%LOCALAPPDATA%\echo`, else `%USERPROFILE%\.cache\echo`
std::filesystem::path user_cache_root()
{
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "echo";
    }

#if defined(_WIN32)
    if (const char *local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        return std::filesystem::path(local) / "echo";
    }

    if (const char *profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return std::filesystem::path(profile) / ".cache" / "echo";
    }
#endif

    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "echo";
    }

    return {};
}

// nothing there at all, or there and holding nothing. An empty directory is adoptable - somebody made the
// path ahead of the build, which is not the mistake this guard is looking for
bool is_empty_or_absent(const std::filesystem::path &directory)
{
    std::error_code ec;

    if (!std::filesystem::exists(directory, ec)) {
        return true;
    }

    return std::filesystem::is_directory(directory, ec) && std::filesystem::is_empty(directory, ec);
}

// does a `relative` result still name something at or below the base it was taken against. Empty means
// the two are unrelated and a leading `..` means it climbed out - the one test behind both containment
// questions here, which differ only in whether the paths they compare exist yet
bool relative_stays_inside(const std::filesystem::path &relative)
{
    // first component, not a byte prefix of `.native()`: on Windows that string is
    // `wchar_t` and `".."` does not convert, while `path("..")` is the same element
    // on every platform
    return !relative.empty() && *relative.begin() != std::filesystem::path("..");
}

};

bool Compiler::is_echo_build_directory(const std::filesystem::path &directory)
{
    std::ifstream in(marker_path(directory), std::ios::binary);
    if (!in) {
        return false;
    }

    std::string first;
    if (!std::getline(in, first) || first.rfind(k_cachedir_signature, 0) != 0) {
        return false;
    }

    for (std::string line; std::getline(in, line);) {
        if (line.rfind(k_echo_marker, 0) == 0) {
            return true;
        }
    }

    return false;
}

bool Compiler::is_compiler_supplied_module(const Parser::ModuleManifest &manifest)
{
    const std::filesystem::path stdlib_root(STDLIB_SOURCE_DIR);

    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(manifest.path, stdlib_root, ec);

    return !ec && relative_stays_inside(relative);
}

bool Compiler::resolve_declared_build_dir(
    const std::string &written,
    const std::filesystem::path &module_dir,
    std::filesystem::path &out_directory,
    std::string &out_reason
)
{
    if (written.empty()) {
        out_reason = "'build_dir' needs a directory name.";
        return false;
    }

    // against the manifest's directory, never the working directory - the rule every other path a manifest
    // holds follows. Not canonicalised: it usually does not exist yet, and normalising the text is all the
    // comparison below needs
    std::filesystem::path resolved(written);

    if (resolved.is_relative()) {
        resolved = module_dir / resolved;
    }

    resolved = resolved.lexically_normal();

    // lexically_normal leaves a trailing separator behind on a `.` or a `foo/`, which reads as an empty
    // filename here and in every message this path later appears in
    if (resolved.filename().empty()) {
        resolved = resolved.parent_path();
    }

    // **a build directory that holds the sources is refused, not adopted.** One that is the module's own
    // directory or an ancestor of it is where the program lives, and `echoc clean` is a command whose whole
    // job is to empty it - so `"."` and `".."` are caught here rather than becoming a directory somebody's
    // checkout disappears into
    if (relative_stays_inside(module_dir.lexically_relative(resolved))) {
        out_reason = fmt::format(
            "'build_dir' names '{}', which holds this module's own sources - a build directory has "
            "to be somewhere the compiler may empty.",
            resolved.string());
        return false;
    }

    out_directory = resolved;
    return true;
}

bool Compiler::is_foreign_build_directory(const std::filesystem::path &directory, BuildDirTrust trust)
{
    // a directory the compiler chose the name of is adopted whatever is in it - an older echoc's artifacts
    // are exactly what is expected there, and refusing them would make an upgrade a build failure
    if (trust != BuildDirTrust::t_asked_for || directory.empty()) {
        return false;
    }

    return !is_empty_or_absent(directory) && !is_echo_build_directory(directory);
}

Compiler::BuildDirState Compiler::prepare_build_directory(
    const std::filesystem::path &directory,
    BuildDirTrust trust
)
{
    if (directory.empty()) {
        return BuildDirState::t_unusable;
    }

    if (is_foreign_build_directory(directory, trust)) {
        return BuildDirState::t_foreign;
    }

    std::error_code ec;

    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return BuildDirState::t_unusable;
    }

    // the marker is the probe: writing it is what proves the directory can take an object
    std::ofstream out(marker_path(directory), std::ios::binary | std::ios::trunc);
    if (!out) {
        return BuildDirState::t_unusable;
    }

    out << marker_contents();

    return out.good() ? BuildDirState::t_ready : BuildDirState::t_unusable;
}

Compiler::BuildDirRemoval Compiler::remove_build_directory(
    const std::filesystem::path &directory,
    BuildDirTrust trust,
    std::string &out_reason
)
{
    out_reason.clear();

    std::error_code ec;

    if (!std::filesystem::exists(directory, ec)) {
        return BuildDirRemoval::t_absent;
    }

    if (trust == BuildDirTrust::t_asked_for && !is_echo_build_directory(directory)) {
        out_reason = fmt::format(
            "'{}' is not a directory echoc created - it carries no 'CACHEDIR.TAG' marker - so nothing "
            "in it was removed.",
            directory.string());
        return BuildDirRemoval::t_refused;
    }

    std::filesystem::remove_all(directory, ec);

    if (ec) {
        out_reason = fmt::format("'{}' could not be removed: {}", directory.string(), ec.message());
        return BuildDirRemoval::t_refused;
    }

    return BuildDirRemoval::t_removed;
}

Compiler::BuildLayout Compiler::BuildLayout::resolve(
    const std::filesystem::path &flag_directory,
    const Parser::ModuleManifest *entry,
    const std::filesystem::path &fallback_scratch
)
{
    BuildLayout layout;
    layout._flag_directory = flag_directory;

    // **the project's own build directory, when there is a project.** A loose .eco file has no manifest and
    // therefore no place of its own to put an object nothing keeps, and the caller's fallback - a directory
    // under the system's temporary path - is the honest answer rather than littering wherever echoc was run
    layout._scratch_is_temporary = entry == nullptr;

    if (layout._scratch_is_temporary) {
        layout._scratch = fallback_scratch;
        return layout;
    }

    // **kept, because the entry module's own directory is one this layout creates and nothing else marks.**
    // It is skipped by the plan - the program is never cached - so the only thing that ever appears inside
    // it is the scratch child, and create_directories makes the parent silently and unmarked. The next
    // build then finds a directory it asked for, holding something, carrying no marker, and refuses itself
    layout._entry_dir = layout.module_dir(*entry);
    layout._entry_trust = layout.trust_of(*entry);
    layout._scratch = layout._entry_dir / "scratch";

    return layout;
}

void Compiler::BuildLayout::discard_temporary_scratch() const
{
    if (!_scratch_is_temporary || _scratch.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::remove_all(_scratch, ec);
}

std::filesystem::path Compiler::BuildLayout::module_dir(const Parser::ModuleManifest &manifest) const
{
    if (!_flag_directory.empty()) {
        // one directory for every module, so the module name has to be in the artifact path rather than only
        // in the filename - two modules could otherwise agree on a key only by agreeing on everything
        return _flag_directory / manifest.name;
    }

    // **the standard library does not build into the compiler's own source tree.**
    //
    // A directory beside the manifest is right for a library that belongs to the program being built: the
    // artifacts travel with the code, and deleting a checkout deletes them. The standard library's manifest
    // lives wherever this compiler was built from, which is nobody's project - writing there means every
    // user's build scribbles in the toolchain, and against an installed compiler that directory is read-only,
    // so the build would fail rather than merely not cache.
    //
    // **above whatever the manifest itself says**, deliberately: that invariant is the compiler's and not the
    // manifest's, and a well-meaning line in stdlib/module.eco is exactly how it would be broken
    if (is_compiler_supplied_module(manifest)) {
        const std::filesystem::path user_root = user_cache_root();

        if (!user_root.empty()) {
            return user_root / manifest.name;
        }
    }

    // the module's own preference over its own artifacts, and never over a dependency's - the one-way rule
    // `#[sources:]` already follows. Already absolute: the manifest reader resolved it against the manifest
    if (!manifest.build_dir.empty()) {
        return manifest.build_dir;
    }

    return manifest.directory / k_default_build_directory;
}

std::filesystem::path Compiler::BuildLayout::module_cc_dir(const Parser::ModuleManifest &manifest) const
{
    return module_dir(manifest) / "cc";
}

std::filesystem::path Compiler::BuildLayout::target_binary(
    const Parser::ModuleManifest &manifest,
    const std::string &target_name
) const
{
    // the module directory itself and not a `bin` under it: the manifest reader has already refused a
    // target name holding a path separator, so the name is one component and the directory stays flat -
    // and a build's output being one `ls` away from the objects it was made of is worth more than the
    // tidiness of a level nobody asked for
    return module_dir(manifest) / target_name;
}

std::filesystem::path Compiler::BuildLayout::scratch_cc_dir() const
{
    return _scratch / "cc";
}

std::filesystem::path Compiler::BuildLayout::scratch_object(const std::string &module_name) const
{
    return _scratch / (module_name + ".o");
}

Compiler::BuildDirTrust Compiler::BuildLayout::trust_of(const Parser::ModuleManifest &manifest) const
{
    // shares module_dir's arm order deliberately, including the compiler-supplied one it skips: that
    // directory is this compiler's own choice whatever the manifest beside it says
    if (!_flag_directory.empty()) {
        return BuildDirTrust::t_asked_for;
    }

    if (!is_compiler_supplied_module(manifest) && !manifest.build_dir.empty()) {
        return BuildDirTrust::t_asked_for;
    }

    return BuildDirTrust::t_ours;
}

Compiler::BuildDirState Compiler::BuildLayout::prepare_module_dir(
    const Parser::ModuleManifest &manifest
) const
{
    return prepare_build_directory(module_dir(manifest), trust_of(manifest));
}

std::string Compiler::BuildLayout::first_foreign_directory(
    const std::vector<Parser::ModuleManifest> &manifests
) const
{
    for (const Parser::ModuleManifest &manifest : manifests) {
        const std::filesystem::path directory = module_dir(manifest);

        if (!is_foreign_build_directory(directory, trust_of(manifest))) {
            continue;
        }

        // it names what to change rather than only what is wrong: the two ways to get here are a
        // `--build-dir` and a `#[build_dir:]`, and which one this module took is exactly what the layout
        // knows and the person reading does not
        return fmt::format(
            "'{}' already holds something, and echoc did not put it there - it carries no 'CACHEDIR.TAG' "
            "marker. A build directory is emptied by 'echoc clean', so nothing was written into it.\n\n"
            "{}",
            directory.string(),
            _flag_directory.empty()
                ? fmt::format(
                    "Point module '{}' somewhere of its own, with '#[build_dir: ...]' in '{}'.",
                    manifest.name, manifest.path.string())
                : fmt::format(
                    "Module '{}' takes its name as a subdirectory of '--build-dir', so give one that has "
                    "nothing of yours under that name.",
                    manifest.name));
    }

    return {};
}

Compiler::BuildDirRemoval Compiler::BuildLayout::remove_module_dir(
    const Parser::ModuleManifest &manifest,
    std::string &out_reason
) const
{
    return remove_build_directory(module_dir(manifest), trust_of(manifest), out_reason);
}

Compiler::BuildDirState Compiler::BuildLayout::prepare_scratch_dir() const
{
    // the entry module's directory first, so it is marked rather than made on the way past - see resolve
    if (!_entry_dir.empty()) {
        const BuildDirState state = prepare_build_directory(_entry_dir, _entry_trust);

        if (state != BuildDirState::t_ready) {
            return state;
        }
    }

    // the scratch directory itself is then either inside one this layout has just vouched for, or a
    // temporary path echoc invented - never one somebody typed
    return prepare_build_directory(_scratch, BuildDirTrust::t_ours);
}
