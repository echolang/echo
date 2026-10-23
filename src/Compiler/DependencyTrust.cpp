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

// `-f<name>` and `-fno-<name>` switches a dependency may set. Nothing here takes a value, so nothing here
// can name a file
constexpr std::array<std::string_view, 34> k_codegen_switches = {
    "PIC", "pic", "PIE", "pie",
    "strict-aliasing", "wrapv", "trapv", "strict-overflow",
    "exceptions", "asynchronous-unwind-tables", "unwind-tables",
    "fast-math", "finite-math-only", "unsafe-math-optimizations", "math-errno", "signed-zeros",
    "omit-frame-pointer", "stack-protector", "stack-protector-strong", "stack-protector-all",
    "common", "builtin", "inline", "inline-functions", "unroll-loops", "vectorize", "slp-vectorize",
    "signed-char", "unsigned-char", "short-enums", "delete-null-pointer-checks",
    "data-sections", "function-sections", "merge-all-constants",
};

// `-f<name>=<value>` options whose value is a word rather than a path
constexpr std::array<std::string_view, 7> k_codegen_options = {
    "visibility=", "fp-contract=", "trivial-auto-var-init=", "sanitize=", "no-sanitize=",
    "ms-compatibility-version=", "max-type-align=",
};

// exactly these spellings
constexpr std::array<std::string_view, 5> k_exact_flags = {
    "-w", "-pedantic", "-pedantic-errors", "-pthread", "-ansi",
};

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
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

bool admits_f_option(std::string_view rest)
{
    for (std::string_view option : k_codegen_options) {
        if (starts_with(rest, option)) {
            return is_plain_word(rest.substr(option.size()));
        }
    }

    const std::string_view name = starts_with(rest, "no-") ? rest.substr(3) : rest;

    return std::find(k_codegen_switches.begin(), k_codegen_switches.end(), name) != k_codegen_switches.end();
}

// a plain word, or `<word>=<word>`: `-Wframe-larger-than=4096`, `-march=armv8-a`
bool is_word_or_setting(std::string_view text)
{
    const size_t equals = text.find('=');

    if (equals == std::string_view::npos) {
        return is_plain_word(text);
    }

    return is_plain_word(text.substr(0, equals)) && is_plain_word(text.substr(equals + 1));
}

bool admits_opt_level(std::string_view flag)
{
    if (flag == "-O" || flag == "-Os" || flag == "-Oz" || flag == "-Og" || flag == "-Ofast") {
        return true;
    }

    return flag.size() == 3 && flag[1] == 'O' && flag[2] >= '0' && flag[2] <= '3';
}

bool admits_debug(std::string_view flag)
{
    if (flag == "-g" || flag == "-ggdb") {
        return true;
    }

    if (flag.size() == 3 && flag[1] == 'g' && std::isdigit(static_cast<unsigned char>(flag[2]))) {
        return true;
    }

    return starts_with(flag, "-gdwarf") && is_plain_word(flag.substr(2));
}

bool admits_flag(std::string_view flag)
{
    if (std::find(k_exact_flags.begin(), k_exact_flags.end(), flag) != k_exact_flags.end()) {
        return true;
    }

    // clang's `-working-directory=` is a path. lowercase `-w`, so it never meets `-W`
    if (starts_with(flag, "-working-directory")) {
        return false;
    }

    // a warning. a comma is the preprocessor/assembler/linker split (`-Wp,`, `-Wa,`, `-Wl,`)
    // and is_word_or_setting already rejects it
    if (starts_with(flag, "-W")) {
        return is_word_or_setting(flag.substr(2));
    }

    if (starts_with(flag, "-O")) {
        return admits_opt_level(flag);
    }

    if (starts_with(flag, "-g")) {
        return admits_debug(flag);
    }

    if (starts_with(flag, "-std=")) {
        return is_plain_word(flag.substr(5));
    }

    // a target feature or a CPU. `-mllvm` and `-mllvm=` hand the rest to LLVM's option parser
    if (starts_with(flag, "-m")) {
        const std::string_view rest = flag.substr(2);

        if (rest == "llvm" || starts_with(rest, "llvm=")) {
            return false;
        }

        return is_word_or_setting(rest);
    }

    if (starts_with(flag, "-f")) {
        return admits_f_option(flag.substr(2));
    }

    if (starts_with(flag, "-D") || starts_with(flag, "-U")) {
        const std::string_view body = flag.substr(2);
        return is_identifier(body.substr(0, body.find('=')));
    }

    return false;
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
    if (admits_flag(flag)) {
        return std::nullopt;
    }

    return fmt::format(
        "the C flag '{}' is not one a dependency may pass. A dependency's '#[cc: flag]' is limited to "
        "warnings, '-std=', '-O', '-g', target options ('-m...'), '-D'/'-U' and a fixed set of '-f' codegen "
        "switches - nothing that loads a plugin, reads a response file, or names a file to read or write. "
        "Include directories go in '#[cc: include]'.",
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
