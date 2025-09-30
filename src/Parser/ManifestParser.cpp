#include "Parser/ManifestParser.h"

#include "Compiler/PhaseTimings.h"

#include "Parser/AttributeParser.h"
#include "Parser/ModuleParser.h"
#include "Parser/SymbolParser.h"

#include "AST/ASTBundle.h"
#include "AST/AttributeNode.h"
#include "AST/LiteralValueNode.h"

#include <glob.hpp>

#include <fmt/core.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace
{

// the attribute names a manifest may use. Anything else is an error rather than an ignored line, which is
// the one rule the whole format rests on: `#[sources: ...]` misspelled `#[source: ...]` would otherwise
// produce a module with no files and no complaint
constexpr std::string_view k_known_attributes[] = { "module", "version", "depends", "sources" };

std::string known_attribute_list()
{
    std::string out;
    for (const std::string_view name : k_known_attributes) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

bool is_known_attribute(const std::string &name)
{
    return std::find(std::begin(k_known_attributes), std::end(k_known_attributes), name)
        != std::end(k_known_attributes);
}

// `<file>:<line>: <what>`, the spelling the .test reader uses. One helper so every message from this file
// is locatable the same way
std::string locate(const std::filesystem::path &path, uint32_t line, const std::string &message)
{
    return fmt::format("{}:{}: {}", path.filename().string(), line, message);
}

std::string locate(const std::filesystem::path &path, const std::string &message)
{
    return fmt::format("{}: {}", path.filename().string(), message);
}

// a `depends` entry may name the manifest or the directory holding it, because both read naturally and
// only one of them survives moving the file
std::optional<std::filesystem::path> manifest_at(const std::filesystem::path &target)
{
    std::error_code ec;

    if (std::filesystem::is_directory(target, ec)) {
        const std::filesystem::path inside = target / "module.eco";
        return std::filesystem::is_regular_file(inside, ec)
            ? std::optional<std::filesystem::path>(inside) : std::nullopt;
    }

    return std::filesystem::is_regular_file(target, ec)
        ? std::optional<std::filesystem::path>(target) : std::nullopt;
}


// true when a path component carries a glob metacharacter
bool has_wildcard(std::string_view text)
{
    return text.find_first_of("*?[") != std::string_view::npos;
}

// the two pattern shapes a source list actually uses, walked without building a regex:
// `<fixed dirs>/*[.ext]` and `<fixed dirs>/**/*[.ext]`. Answers nullopt for anything else, and
// expand_source_pattern falls back to glob - so the grammar accepted is glob's, only the cost differs.
//
// a leading dot is not matched, which is what glob does with `*` too - a dotfile beside the sources is not
// one of them
std::optional<std::vector<std::filesystem::path>> expand_directory_pattern(
    const std::filesystem::path &absolute_pattern, bool recursive)
{
    std::filesystem::path parent = absolute_pattern.parent_path();
    const std::string leaf = absolute_pattern.filename().string();

    // the leaf must be `*` or `*.<ext>`, with the extension itself literal
    if (leaf != "*" && !(leaf.rfind("*.", 0) == 0 && !has_wildcard(leaf.substr(2)))) {
        return std::nullopt;
    }

    // a recursive pattern spells its descent as a `**` component immediately above the leaf
    if (recursive) {
        if (parent.filename() != "**") {
            return std::nullopt;
        }
        parent = parent.parent_path();
    }

    if (has_wildcard(parent.string())) {
        return std::nullopt;
    }

    const std::string extension = (leaf == "*") ? std::string() : leaf.substr(1);

    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        return std::vector<std::filesystem::path>{};
    }

    std::vector<std::filesystem::path> matches;

    const auto consider = [&](const std::filesystem::directory_entry &entry) {
        const std::string name = entry.path().filename().string();

        if (name.empty() || name[0] == '.') {
            return;
        }

        if (!extension.empty() && entry.path().extension() != extension) {
            return;
        }

        matches.push_back(entry.path());
    };

    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(parent, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                return std::nullopt;
            }
            consider(*it);
        }
    }
    else {
        for (std::filesystem::directory_iterator it(parent, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                return std::nullopt;
            }
            consider(*it);
        }
    }

    return matches;
}

std::filesystem::path canonical_or_absolute(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::canonical(path, ec);
    return ec ? std::filesystem::absolute(path) : resolved;
}

// the scratch state reading a manifest needs, built **once for a whole graph** rather than once per
// manifest.
//
// per manifest is what this was, and it dominated the entire front end: a Parser::ModuleParser builds a
// Lexer, whose constructor registers 74 token matchers, and an AST::Bundle brings up a namespace tree, an
// operator registry and a type registry. Reading one 6-line manifest therefore cost more than lexing and
// parsing the whole standard library. `--timings` is what found it, on the first run of the flag
struct ManifestScratch
{
    AST::Bundle bundle;
    Parser::ModuleParser parser;
    size_t next_module = 0;

    // a *fresh module* per manifest even so, because the attributes are read back out of the module's own
    // NodeCollection - reusing one would hand the second manifest every attribute the first declared.
    // The collector is deliberately shared: a manifest declares nothing into it
    AST::Module &fresh_module()
    {
        AST::module_handle_t handle =
            bundle.modules.add_module(fmt::format("manifest${}", next_module++));
        return bundle.modules.get_module(handle);
    }
};

};

std::vector<std::filesystem::path> Parser::expand_source_pattern(const std::filesystem::path &pattern)
{
    const std::string spelled = pattern.string();
    const bool recursive = spelled.find("**") != std::string::npos;

    if (std::optional<std::vector<std::filesystem::path>> walked =
            expand_directory_pattern(pattern, recursive)) {
        return std::move(walked.value());
    }

    return recursive ? glob::rglob(spelled) : glob::glob(spelled);
}

namespace
{

// the critical issues the parser recorded for *this* manifest, located, as one message - empty when it
// parsed. A manifest that does not lex or parse fails on the parser's own messages, which are better than
// anything this file could invent
std::string manifest_parse_errors(
    const AST::Collector &collector, size_t issues_before, const std::filesystem::path &path)
{
    std::string reported;

    for (size_t i = issues_before; i < collector.issues.size(); i++) {
        const auto &issue = collector.issues[i];

        if (!issue->is_critical()) {
            continue;
        }

        reported += locate(path, std::get<0>(issue->code_ref.line_range()), issue->message());
        reported += "\n";
    }

    return reported;
}

// the attribute values, straight off the nodes the declaration pass collected. `sources` and `depends` come
// back as written, because both resolve against the manifest's directory and neither can be checked here.
//
// takes the payload rather than the module alone: attribute_string_value reports through it, so the values
// have to be read while it is alive
bool read_manifest_attributes(
    Parser::Payload &payload,
    AST::Module &module,
    Parser::ModuleManifest &out,
    std::vector<std::string> &out_sources,
    std::vector<std::string> &out_depends,
    std::string &out_error)
{
    // read from the arena rather than from the file root: the root is only built by the body pass, and the
    // arena preserves the order the attributes were written in
    for (AST::AttributeNode *attribute : module.nodes.of_type<AST::AttributeNode>()) {
        const std::string name = attribute->attribute_id.value();
        const uint32_t line = attribute->attribute_id.line();

        if (!is_known_attribute(name)) {
            out_error = locate(out.path, line, fmt::format(
                "unknown manifest attribute '{}', expected one of: {}", name, known_attribute_list()));
            return false;
        }

        std::optional<std::string> value = Parser::attribute_string_value(payload, attribute, name);
        if (!value.has_value()) {
            out_error = locate(out.path, line,
                fmt::format("the '{}' attribute needs exactly one string value.", name));
            return false;
        }

        if (name == "module") {
            if (!out.name.empty()) {
                out_error = locate(out.path, line, "'module' is declared twice.");
                return false;
            }
            out.name = value.value();
        }
        else if (name == "version") {
            if (!out.version.empty()) {
                out_error = locate(out.path, line, "'version' is declared twice.");
                return false;
            }
            out.version = value.value();
        }
        else if (name == "depends") {
            out_depends.push_back(value.value());
        }
        else if (name == "sources") {
            out_sources.push_back(value.value());
        }
    }

    if (out.name.empty()) {
        out_error = locate(out.path, "no '#[module: \"...\"]' - a manifest has to name its module.");
        return false;
    }

    // the name becomes an AST module name, which codegen requires to be unique across the bundle and
    // the mangler never reads - so the only rule it needs is that it is spellable
    if (out.name.find_first_of(" \t\"") != std::string::npos) {
        out_error = locate(out.path,
            fmt::format("'{}' is not a usable module name - no spaces or quotes.", out.name));
        return false;
    }

    return true;
}

// out.sources, from the patterns as written. **relative to the manifest, never to the working directory**
bool expand_manifest_sources(
    const std::vector<std::string> &patterns, Parser::ModuleManifest &out, std::string &out_error)
{
    if (patterns.empty()) {
        out_error = locate(out.path,
            "no '#[sources: \"...\"]' - a manifest has to say which files the module is made of.");
        return false;
    }

    Compiler::ScopedPhase glob_phase("sources glob");

    std::error_code ec;
    std::set<std::filesystem::path> unique_sources;

    for (const std::string &pattern : patterns) {
        size_t kept = 0;

        for (const std::filesystem::path &match : Parser::expand_source_pattern(out.directory / pattern)) {
            if (!std::filesystem::is_regular_file(match, ec)) {
                continue;
            }

            const std::filesystem::path resolved = canonical_or_absolute(match);

            // **a manifest is never one of its own sources.** It is an Echo file living in the directory it
            // describes, so `#[sources: "*.eco"]` matches it - and compiling it would declare its own
            // attributes into the program being built. Excluding it here rather than asking authors to write
            // a pattern that avoids it: the obvious pattern is the one that breaks
            if (resolved == out.path) {
                continue;
            }

            unique_sources.insert(resolved);
            kept++;
        }

        // **a pattern matching nothing is an error here**, unlike a wildcard on the command line. On the
        // command line an empty glob is a pattern the user typed loosely; in a manifest it is a declaration
        // that this module is made of files, and finding none of them means the module is silently empty
        if (kept == 0) {
            out_error = locate(out.path,
                fmt::format("the sources pattern '{}' matched no files.", pattern));
            return false;
        }
    }

    // sorted, so the module's token indices - and so every symbol minted from a source position - do not
    // depend on the order the patterns were written in
    out.sources.assign(unique_sources.begin(), unique_sources.end());

    return true;
}

// out.depends, canonical. An entry may name the manifest or the directory holding it - see manifest_at
bool resolve_manifest_depends(
    const std::vector<std::string> &written, Parser::ModuleManifest &out, std::string &out_error)
{
    for (const std::string &spelled : written) {
        const std::filesystem::path target = out.directory / spelled;
        const std::optional<std::filesystem::path> resolved = manifest_at(target);

        if (!resolved.has_value()) {
            out_error = locate(out.path, fmt::format(
                "the dependency '{}' resolves to '{}', which is neither a manifest nor a directory "
                "holding a 'module.eco'.", spelled, target.string()));
            return false;
        }

        out.depends.push_back(canonical_or_absolute(resolved.value()));
    }

    return true;
}

// the body of the read, taking the scratch state so a graph walk can reuse it
bool read_manifest_with(
    ManifestScratch &scratch,
    const std::filesystem::path &path,
    Parser::ModuleManifest &out,
    std::string &out_error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        out_error = fmt::format("{}: no such manifest file.", path.string());
        return false;
    }

    out = Parser::ModuleManifest{};
    out.path = canonical_or_absolute(path);
    out.directory = out.path.parent_path();

    // a throwaway module and a throwaway collector: the manifest declares nothing, and parsing it into the
    // bundle being built would put its attributes into the program's own tree
    AST::Module &module = scratch.fresh_module();

    // where this manifest's own issues start. The collector is shared across a graph walk, so "did *this*
    // manifest parse" cannot be asked as has_critical_issues() - that answers for every manifest read so
    // far. Today a critical issue aborts the walk immediately so the two agree, but that is a property of
    // the caller rather than of this check, and it is not one worth depending on
    const size_t issues_before = scratch.bundle.collector.issues.size();

    AST::File &file = module.add_file(out.path);

    if (!file.read_from_disk(out_error)) {
        return false;
    }

    Parser::ModuleParser &parser = scratch.parser;

    AST::TokenizedFile tokenized = parser.make_tokenized_file(module, file);

    // passes 1 and 2 only. A file-scope attribute is collected by the declaration pass, so the body pass
    // would add nothing but a second copy of every attribute node
    {
        Parser::Payload payload =
            parser.make_parser_payload(tokenized, module, scratch.bundle.collector, Parser::Pass::t_type_names);
        Parser::parse_type_names(payload);
    }

    std::vector<std::string> sources_written;
    std::vector<std::string> depends_written;

    {
        Parser::Payload payload =
            parser.make_parser_payload(tokenized, module, scratch.bundle.collector, Parser::Pass::t_declarations);
        Parser::parse_symbols(payload);

        std::string reported =
            manifest_parse_errors(scratch.bundle.collector, issues_before, out.path);

        if (!reported.empty()) {
            out_error = std::move(reported);
            return false;
        }

        if (!read_manifest_attributes(
                payload, module, out, sources_written, depends_written, out_error)) {
            return false;
        }
    }

    return expand_manifest_sources(sources_written, out, out_error)
        && resolve_manifest_depends(depends_written, out, out_error);
}

};

bool Parser::read_module_manifest(
    const std::filesystem::path &path, Parser::ModuleManifest &out, std::string &out_error)
{
    ManifestScratch scratch;
    return read_manifest_with(scratch, path, out, out_error);
}

bool Parser::resolve_module_graph(
    const std::vector<std::filesystem::path> &roots,
    std::vector<Parser::ModuleManifest> &out,
    std::string &out_error)
{
    out.clear();

    // one scratch for the whole reachable set - see ManifestScratch
    ManifestScratch scratch;

    std::map<std::filesystem::path, ModuleManifest> loaded;

    // read the whole reachable set first, then order it. Two phases because a cycle has to be reported as a
    // cycle rather than as a stack overflow, and because a diamond must read each manifest once
    std::vector<std::filesystem::path> pending;

    // the roots in the order they were given, kept for the emit order below
    std::vector<std::filesystem::path> root_order;

    for (const std::filesystem::path &root : roots) {
        const std::optional<std::filesystem::path> resolved = manifest_at(root);
        if (!resolved.has_value()) {
            out_error = fmt::format(
                "{}: no such manifest - expected a manifest file or a directory holding a 'module.eco'.",
                root.string());
            return false;
        }
        pending.push_back(canonical_or_absolute(resolved.value()));
        root_order.push_back(pending.back());
    }

    while (!pending.empty()) {
        const std::filesystem::path next = pending.back();
        pending.pop_back();

        if (loaded.count(next) > 0) {
            continue;
        }

        ModuleManifest manifest;
        if (!read_manifest_with(scratch, next, manifest, out_error)) {
            return false;
        }

        for (const std::filesystem::path &dependency : manifest.depends) {
            pending.push_back(dependency);
        }

        loaded.emplace(next, std::move(manifest));
    }

    // two modules cannot share a name: codegen keys its compilation units on it, and a duplicate would be
    // caught much later by create_cmp_units with no manifest to point at
    std::map<std::string, std::filesystem::path> by_name;
    for (const auto &[path, manifest] : loaded) {
        auto [existing, inserted] = by_name.emplace(manifest.name, path);
        if (!inserted) {
            out_error = fmt::format(
                "two manifests declare the module name '{}': '{}' and '{}'. Module names must be unique "
                "within a build.", manifest.name, existing->second.string(), path.string());
            return false;
        }
    }

    // depth-first, emitting a module after everything it depends on. `in_progress` is what turns a cycle
    // into a named path instead of an infinite descent
    std::set<std::filesystem::path> emitted;
    std::vector<std::filesystem::path> in_progress;

    std::function<bool(const std::filesystem::path &)> visit =
        [&](const std::filesystem::path &path) {
            if (emitted.count(path) > 0) {
                return true;
            }

            if (std::find(in_progress.begin(), in_progress.end(), path) != in_progress.end()) {
                std::string chain;
                for (const std::filesystem::path &step : in_progress) {
                    chain += loaded.at(step).name;
                    chain += " -> ";
                }
                chain += loaded.at(path).name;

                out_error = fmt::format(
                    "the module dependencies form a cycle: {}. A module is parsed completely before the "
                    "next one starts, so it can only name symbols from modules parsed before it - which "
                    "makes a cycle unsatisfiable rather than merely unsupported.", chain);
                return false;
            }

            in_progress.push_back(path);

            for (const std::filesystem::path &dependency : loaded.at(path).depends) {
                if (!visit(dependency)) {
                    return false;
                }
            }

            in_progress.pop_back();
            emitted.insert(path);
            out.push_back(loaded.at(path));

            return true;
        };

    // **the roots in the order they were given, first.**
    //
    // This used to walk `loaded`, a path-keyed map, on the theory that a canonical-path order is more
    // reproducible than a command line. It is reproducible and it is also wrong: two modules that do not
    // depend on each other are still ordered relative to each other, because a module can name symbols from
    // any module parsed before it. So the order is part of what a build *means*, and the caller is the only
    // thing that knows it - the standard library is passed as the first root precisely because everything may
    // use it.
    //
    // deriving it from the filesystem instead made that depend on where a project happened to sit on disk: a
    // library using `string` compiled from /tmp and failed from a directory sorting above `stdlib/`, because
    // the standard library was ordered after it. Same source, same command, different answer.
    //
    // walking the roots is also the whole walk: everything in `loaded` got there as a root or as some
    // manifest's `depends`, so the recursion above reaches all of it. A sweep over `loaded` afterwards would
    // be inert *and* would be a second, filesystem-ordered emit path for a reader to rule out
    for (const std::filesystem::path &root : root_order) {
        if (!visit(root)) {
            out.clear();
            return false;
        }
    }

    return true;
}
