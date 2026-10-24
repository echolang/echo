#include "Compiler/DependencyTrust.h"

#include "Compiler/BuildLayout.h"
#include "Compiler/ParsePipeline.h"
#include "Compiler/SettledPath.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace
{

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

bool contains_path_chars(std::string_view text)
{
    return text.find('/') != std::string_view::npos
        || text.find('\\') != std::string_view::npos
        || text.find("..") != std::string_view::npos;
}

// `prefix` as a flag, glued path, `=value`, or a `-` continuation (`-fplugin-arg`, `-include-pch`).
// a following letter is a *different* flag (`-include` must not claim `-incremental-linker-compatible`)
bool matches_flag_prefix(std::string_view flag, std::string_view prefix)
{
    if (flag == prefix) {
        return true;
    }

    if (!starts_with(flag, prefix) || flag.size() == prefix.size()) {
        return false;
    }

    const char next = flag[prefix.size()];
    return next == '=' || next == '/' || next == '\\' || next == '-' || next == ',';
}

// clang's one-letter path flags: `-I`, `-F`, `-B`, `-L`. case-sensitive, so `-fobjc-arc` is not `-F`
bool is_letter_path_flag(std::string_view flag, char letter)
{
    return flag.size() >= 2 && flag[0] == '-' && flag[1] == letter;
}

// plugins, response files, tool passthrough, path-valued flags, outputs. longest prefixes first
bool refuses_flag(std::string_view flag)
{
    if (flag.empty() || flag.front() == '@') {
        return true;
    }

    static constexpr std::array<std::string_view, 8> k_plugins = {
        "-fpass-plugin", "-fplugin", "-Xpreprocessor", "-Xassembler",
        "-Xclang", "-Xlinker", "-mllvm", "-load",
    };

    for (std::string_view prefix : k_plugins) {
        if (matches_flag_prefix(flag, prefix)) {
            return true;
        }
    }

    if (starts_with(flag, "-Wl,") || starts_with(flag, "-Wa,") || starts_with(flag, "-Wp,")) {
        return true;
    }

    static constexpr std::array<std::string_view, 12> k_paths = {
        "--serialize-diagnostics", "--working-directory", "-working-directory",
        "--sysroot", "-isysroot", "-idirafter", "-iframework", "-isystem",
        "-include", "-iquote", "--config", "-dumpdir",
    };

    for (std::string_view prefix : k_paths) {
        if (matches_flag_prefix(flag, prefix)) {
            return true;
        }
    }

    if (is_letter_path_flag(flag, 'I')
        || is_letter_path_flag(flag, 'F')
        || is_letter_path_flag(flag, 'B')
        || is_letter_path_flag(flag, 'L')) {
        return true;
    }

    // GNU ld treats `-l` with a `/` as a pathname; a library name is `#[link: lib]`
    if (starts_with(flag, "-l") && contains_path_chars(flag.substr(2))) {
        return true;
    }

    // exact `-o` and a glued path. `-openmp` / `-objc` keep a letter after `-o`
    if (flag == "-o"
        || starts_with(flag, "-o=")
        || starts_with(flag, "-o/")
        || starts_with(flag, "-o\\")) {
        return true;
    }

    static constexpr std::array<std::string_view, 6> k_outputs = {
        "-MF", "-MT", "-MQ", "-MJ", "-save-temps", "-gen-reproducer",
    };

    for (std::string_view prefix : k_outputs) {
        if (starts_with(flag, prefix)) {
            return true;
        }
    }

    const size_t equals = flag.find('=');

    if (equals != std::string_view::npos && contains_path_chars(flag.substr(equals + 1))) {
        return true;
    }

    return false;
}

// a word clang takes as a name: letters, digits, `_`, `-`, `+`, `.`. Never a separator, so never a path
bool is_plain_word(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+' || c == '.';
    });
}

bool is_identifier(std::string_view text)
{
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front()))) {
        return false;
    }

    return std::all_of(text.begin(), text.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

// a library, framework, or `file:` name a dependency may hand the linker: one component, never a path.
// GNU ld treats `-l` with a `/` (or a leading `:`) as a pathname; `echoc run` dlopens an absolute `file:`
bool is_linker_name(std::string_view text)
{
    if (text.empty() || text.front() == ':' || text == "." || text == "..") {
        return false;
    }

    if (text.find('/') != std::string_view::npos || text.find('\\') != std::string_view::npos
        || text.find("..") != std::string_view::npos) {
        return false;
    }

    return is_plain_word(text);
}

std::optional<std::string> path_escape(
    const Parser::ModuleManifest &manifest,
    const std::filesystem::path &path,
    std::string_view noun
)
{
    if (Compiler::path_is_inside(path, manifest.directory)) {
        return std::nullopt;
    }

    return fmt::format(
        "the {} '{}' of module '{}' is outside that module's directory '{}'. A dependency is "
        "built from files of its own, so a pattern that reaches past its manifest - an absolute "
        "path, a '..', or a symbolic link pointing out - is refused.",
        noun, path.string(), manifest.name, manifest.directory.string());
}

std::optional<std::string> source_escape(
    const Parser::ModuleManifest &manifest,
    const std::vector<std::filesystem::path> &sources,
    std::string_view noun
)
{
    for (const std::filesystem::path &source : sources) {
        if (std::optional<std::string> escape = path_escape(manifest, source, noun)) {
            return escape;
        }
    }

    return std::nullopt;
}

};

bool Compiler::module_is_trusted(
    const Parser::ModuleManifest &manifest,
    const std::vector<std::filesystem::path> &roots
)
{
    return is_compiler_supplied_module(manifest) || manifest_is_a_root(manifest, roots);
}

std::optional<std::string> Compiler::dependency_cc_flag_refusal(const std::string &flag)
{
    if (!refuses_flag(flag)) {
        return std::nullopt;
    }

    return fmt::format(
        "the C flag '{}' is not one a dependency may pass. A dependency's '#[cc: flag]' cannot load a "
        "plugin, read a response file, or name a file to read or write. Include directories go in "
        "'#[cc: include]'.",
        flag);
}

std::optional<std::string> Compiler::dependency_cc_define_refusal(const std::string &define)
{
    const std::string name = define.substr(0, define.find('='));

    if (is_identifier(name)) {
        return std::nullopt;
    }

    return fmt::format("'{}' is not a C macro name, so '#[cc: define]' cannot define it.", name);
}

std::optional<std::string> Compiler::dependency_contribution_refusal(
    const Parser::ModuleManifest &manifest,
    const Parser::ModuleContribution &contribution
)
{
    if (std::optional<std::string> escape = source_escape(manifest, contribution.sources, "source")) {
        return escape;
    }

    if (std::optional<std::string> escape = source_escape(manifest, contribution.cc.sources, "C source")) {
        return escape;
    }

    if (std::optional<std::string> escape =
            source_escape(manifest, contribution.cc.includes, "include directory")) {
        return escape;
    }

    for (const Compiler::LinkRequirement &requirement : contribution.link) {
        if (requirement.scheme == Compiler::LinkScheme::t_object) {
            if (std::optional<std::string> escape =
                    path_escape(manifest, requirement.value, "object")) {
                return escape;
            }
        } else if (requirement.scheme == Compiler::LinkScheme::t_search) {
            if (std::optional<std::string> escape =
                    path_escape(manifest, requirement.value, "search directory")) {
                return escape;
            }
        } else if (requirement.scheme == Compiler::LinkScheme::t_library
            || requirement.scheme == Compiler::LinkScheme::t_framework) {
            if (!is_linker_name(requirement.value)) {
                return fmt::format(
                    "the {} '{}' of module '{}' is not a library name. A dependency may only "
                    "name a system library, never a path the linker or the runtime would open.",
                    requirement.scheme == Compiler::LinkScheme::t_library ? "library" : "framework",
                    requirement.value, manifest.name);
            }

            if (requirement.file.has_value() && !is_linker_name(requirement.file.value())) {
                return fmt::format(
                    "the loadable file '{}' of module '{}' is not a file name. Write a SONAME "
                    "(`libssl.so.3`), not a path.",
                    requirement.file.value(), manifest.name);
            }
        }
    }

    for (const std::string &flag : contribution.cc.flags) {
        if (std::optional<std::string> refusal = dependency_cc_flag_refusal(flag)) {
            return fmt::format("module '{}': {}", manifest.name, refusal.value());
        }
    }

    for (const std::string &define : contribution.cc.defines) {
        if (std::optional<std::string> refusal = dependency_cc_define_refusal(define)) {
            return fmt::format("module '{}': {}", manifest.name, refusal.value());
        }
    }

    return std::nullopt;
}
