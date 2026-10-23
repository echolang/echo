#include "Parser/ManifestParser.h"

#include "Compiler/BuildLayout.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/SettledPath.h"

#include "Parser/AttributeParser.h"
#include "Parser/ManifestHooks.h"
#include "Parser/ManifestPackage.h"
#include "Parser/ManifestScope.h"
#include "Parser/ManifestTargets.h"
#include "Parser/ModuleParser.h"
#include "Parser/SymbolParser.h"

#include "AST/ASTAttributes.h"
#include "AST/ASTBundle.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/AttributeNode.h"
#include "AST/LiteralValueNode.h"
#include "Parser/AttributeParser.h"

#include <glob.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace
{

// the attribute names a manifest may use are AST::attribute_admission's, asked with
// AttributeMedium::t_manifest. Anything else is an error rather than an ignored line,
// which is the one rule the whole format rests on: `#[sources: ...]` misspelled `#[source: ...]` would
// otherwise produce a module with no files and no complaint

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

};

std::optional<std::filesystem::path> Parser::manifest_at(const std::filesystem::path &target)
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

using Parser::ManifestReport;
using Parser::WrittenHook;
using Parser::WrittenScope;
using Parser::WrittenTarget;
using Parser::read_manifest_hooks;
using Parser::read_manifest_targets;
using Parser::report_at;
using Parser::resolve_scope_edges;
using Parser::settle_manifest_hooks;
using Parser::is_usable_artifact_name;

template <typename Issue, typename... Args>
void report_span(const ManifestReport &into, const TokenSpan &span, Args... args)
{
    if (!span.is_valid()) {
        report_at<Issue>(into, 1, args...);
        return;
    }

    into.collector.collect_issue<Issue>(
        AST::CodeRef { &into.module, span.slice() },
        args...);
}

// the attribute values, straight off the nodes the declaration pass collected. `sources` and `depends` come
// back as written, because both resolve against the manifest's directory and neither can be checked here.
// `#[depends: ...]`, in every shape it takes.
//
//     #[depends: "../geom"]                                a path, the common case
//     #[depends: path "../geom"]                           the same, said out loud
//     #[depends: ["../geom", "../core"]]                   several
//     #[depends: git { url: "https://...", rev: "v1" }]    a **fixed** record - refused, see below
//
// **a bare string still means a path**, so the tag is what a second kind needs rather than what the
// first one costs. `git` is parsed and validated and then refused, because nothing fetches one yet:
// accepting it silently would give a build a dependency it never got, and leaving the tag out entirely
// would report it as an unknown scheme, which is the wrong sentence for a thing that is merely not
// built yet
// refuses through `reader` rather than answering, so the caller drains it the one way every other
// attribute here is drained - a second success channel is a second thing to forget to check
void read_manifest_depends(
    const AST::AttributeValue &written,
    AST::AttributeReader &reader,
    std::vector<std::string> &out_depends)
{
    for (const AST::AttributeValue *entry : AST::AttributeReader::each(written)) {
        if (entry->has_tag() && entry->tag() == "git") {
            if (!reader.record(*entry)) {
                continue;
            }

            reader.require_field(*entry, "url");
            reader.reject_unknown_fields(*entry, { "url", "rev" });

            if (reader.has_refusals()) {
                continue;
            }

            reader.refuse(entry->span,
                "git dependencies are not resolved yet - write '#[requires: \"name\" { version: \"...\", "
                "source: git \"...\" }]' and run `epm install`, or vendor the module and name it with a path.");
            continue;
        }

        if (entry->has_tag() && entry->tag() != "path") {
            reader.refuse(entry->tag_span, fmt::format(
                "'{}' is not a kind of dependency, expected one of: path, git.", entry->tag()));
            continue;
        }

        if (std::optional<std::string> value = reader.string(*entry)) {
            out_depends.push_back(value.value());
        }
    }
}


// takes the payload rather than the module alone: it reports through the collector, so the values have
// to be read while it is alive
bool read_manifest_attributes(
    Parser::Payload &payload,
    AST::Module &module,
    const Compiler::TargetFacts &facts,
    Parser::ModuleManifest &out,
    std::vector<std::string> &out_sources,
    std::vector<std::string> &out_depends,
    std::vector<WrittenTarget> &out_targets,
    std::vector<WrittenHook> &out_hooks)
{
    // which written target or hook an attribute's `{ ... }` fills. **The arena order is what makes one pass
    // enough**: an owner is parsed before anything written inside it, so its slot is always registered by
    // the time a child asks for it
    struct ScopeSlot
    {
        AST::ManifestScopeOwner kind = AST::ManifestScopeOwner::t_target;
        size_t index = 0;
    };

    const auto written_scope_of = [&out_targets, &out_hooks](const ScopeSlot &slot) -> WrittenScope & {
        if (slot.kind == AST::ManifestScopeOwner::t_hook) {
            return out_hooks[slot.index].scope;
        }

        return out_targets[slot.index].scope;
    };

    std::unordered_map<const AST::AttributeNode *, ScopeSlot> scope_of;

    // a `{ }` belongs to one owner. the two arms that open a scope share this rather than copying the
    // count check; `has_scope` is a target's field and is set by the target arm after a successful claim
    const auto claim_opened_scope = [&](
        AST::AttributeNode *attribute,
        const AST::AttributeReader &reader,
        AST::ManifestScopeOwner owner,
        size_t added,
        size_t index
    ) {
        if (!attribute->opens_scope || reader.has_refusals()) {
            return true;
        }

        if (added != 1) {
            payload.collector.collect_issue<AST::Issue::InvalidManifestScope>(
                payload.context.code_ref(attribute->attribute_id),
                fmt::format(
                    "a '{{ ... }}' scope belongs to one {}, and this '#[{}: ...]' declares "
                    "{} - write each of them its own.",
                    AST::manifest_scope_owner_name(owner), AST::manifest_scope_owner_name(owner), added));
            return false;
        }

        scope_of[attribute] = ScopeSlot{ owner, index };
        return true;
    };

    // read from the arena rather than from the file root: the root is only built by the body pass, and the
    // arena preserves the order the attributes were written in
    for (AST::AttributeNode *attribute : module.nodes.of_type<AST::AttributeNode>()) {
        const std::string name = attribute->attribute_id.value();
        const uint32_t line = attribute->attribute_id.line();

        switch (AST::attribute_admission(name, AST::AttributeMedium::t_manifest, false)) {
        case AST::AttributeAdmission::t_language:
        case AST::AttributeAdmission::t_tool:
            break;

        case AST::AttributeAdmission::t_reserved_namespace:
            payload.collector.collect_issue<AST::Issue::ReservedAttributeNamespace>(
                payload.context.code_ref(attribute->attribute_id),
                "the 'echoc::' namespace is reserved for the compiler and its standard library.");
            return false;

        case AST::AttributeAdmission::t_compiler:
        case AST::AttributeAdmission::t_unknown_compiler:
        case AST::AttributeAdmission::t_manifest_in_source:
        case AST::AttributeAdmission::t_unknown:
            payload.collector.collect_issue<AST::Issue::UnknownManifestAttribute>(
                payload.context.code_ref(attribute->attribute_id),
                fmt::format(
                    "unknown manifest attribute '{}', expected one of: {}",
                    name, AST::known_manifest_attribute_list()));
            return false;
        }

        // **every scope rule is this one table lookup**, asked of a name the vocabulary already accepted.
        // Three name comparisons spread through the loop is what this replaced, and the fourth name they
        // were missing was `target` itself: one written *inside* a scope carries no brace of its own, so
        // the nesting rule never saw it and the arm below hung a second module-level target off it
        const AST::AttributeScoping scoping = AST::manifest_attribute_scoping(name);

        // asked of the attribute carrying the brace, and before its value is read: that is where the line
        // number is, and it is the only place an *empty* scope is visible at all - nobody names the owner
        // of a scope with nothing in it
        if (attribute->opens_scope && scoping != AST::AttributeScoping::t_opens_a_scope) {
            payload.collector.collect_issue<AST::Issue::InvalidManifestScope>(
                payload.context.code_ref(attribute->attribute_id),
                fmt::format(
                    "'{}' cannot carry a '{{ ... }}' scope - only a '#[target: ...]' or a '#[hook: ...]' "
                    "can, a scope being what one of them says for itself.", name));
            return false;
        }

        // where this attribute's value goes: the module when null, or the target or hook whose scope it was
        // written in. Resolved here and used at once - a later push_back may move the vector it points into
        WrittenScope *scope = nullptr;

        if (attribute->scope_owner != nullptr) {
            if (scoping == AST::AttributeScoping::t_opens_a_scope) {
                payload.collector.collect_issue<AST::Issue::InvalidManifestScope>(
                    payload.context.code_ref(attribute->attribute_id),
                    fmt::format(
                        "'{}' cannot be written inside a '{{ ... }}' scope - scopes do not nest, so a "
                        "target or a hook is declared at file scope.", name));
                return false;
            }

            // `.at()` rather than a lookup with a refusal behind it: the owner opened a scope, and an
            // attribute that opens one either registers a slot below or returns. That is an invariant of
            // this walk, so it fails as one rather than as a sentence about somebody's manifest
            const ScopeSlot &slot = scope_of.at(attribute->scope_owner);

            // **a second column of the table, asked rather than branched on**: what a scope may hold is a
            // question about its owner as well as about the attribute
            if (!AST::manifest_scope_admits(slot.kind, name)) {
                payload.collector.collect_issue<AST::Issue::InvalidManifestScope>(
                    payload.context.code_ref(attribute->attribute_id),
                    scoping == AST::AttributeScoping::t_module_only
                        ? fmt::format(
                            "'{}' describes the module, not one of its targets - write it at file scope.",
                            name)
                        : fmt::format(
                            "'{}' cannot be written inside a '#[{}: ...]' scope - it holds only {}.",
                            name, AST::manifest_scope_owner_name(slot.kind),
                            AST::manifest_scope_admits_list(slot.kind)));
                return false;
            }

            scope = &written_scope_of(slot);
        }

        if (!attribute->value.has_value()) {
            payload.collector.collect_issue<AST::Issue::InvalidAttributeValue>(
                payload.context.code_ref(attribute->attribute_id),
                fmt::format("the '{}' attribute needs a value - write '#[{}: ...]'.", name, name));
            return false;
        }

        const AST::AttributeValue &written = attribute->value.value();

        AST::AttributeReader reader(name);

        if (name == "module") {
            if (!out.name.empty()) {
                payload.collector.collect_issue<AST::Issue::RepeatedManifestAttribute>(
                    payload.context.code_ref(attribute->attribute_id),
                    "'module' is declared twice.");
                return false;
            }

            if (std::optional<std::string> value = reader.string(written)) {
                out.name = value.value();
            }
        }
        else if (name == "version") {
            if (!out.version.empty()) {
                payload.collector.collect_issue<AST::Issue::RepeatedManifestAttribute>(
                    payload.context.code_ref(attribute->attribute_id),
                    "'version' is declared twice.");
                return false;
            }

            if (std::optional<std::string> value = reader.string(written)) {
                out.version = value.value();
            }
        }
        else if (name == "depends") {
            read_manifest_depends(written, reader, scope != nullptr ? scope->scoped_depends : out_depends);
        }
        else if (name == "requires") {
            Parser::read_manifest_requires(
                written, reader, scope != nullptr ? scope->scoped_requirements : out.requirements);
        }
        else if (name == "sources") {
            std::vector<std::string> &into = scope != nullptr ? scope->scoped_sources : out_sources;

            for (const AST::AttributeValue *pattern : AST::AttributeReader::each(written)) {
                if (std::optional<std::string> value = reader.string(*pattern)) {
                    into.push_back(value.value());
                }
            }
        }
        else if (name == "target") {
            // checked against `sources` after the patterns are expanded - resolve_manifest_targets, which
            // is the first moment there is anything to check an entry against
            const size_t before = out_targets.size();

            read_manifest_targets(written, reader, line, out_targets);

            if (!claim_opened_scope(
                    attribute, reader, AST::ManifestScopeOwner::t_target, out_targets.size() - before,
                    before)) {
                return false;
            }

            if (attribute->opens_scope && !reader.has_refusals()) {
                out_targets[before].has_scope = true;
            }
        }
        else if (name == "hook") {
            const size_t before = out_hooks.size();

            read_manifest_hooks(written, reader, line, out_hooks);

            if (!claim_opened_scope(
                    attribute, reader, AST::ManifestScopeOwner::t_hook, out_hooks.size() - before, before)) {
                return false;
            }
        }
        else if (name == "link") {
            // **the scheme is checked here, against this invocation's facts.** A `framework:` on a linux
            // build is a mistake in the manifest and refusing it needs the platform, which the reader has
            // and nothing downstream does - by link time there is only a word left
            Compiler::parse_link_attribute(
                written, out.directory, facts, out.name, reader,
                scope != nullptr ? scope->scoped_link : out.link);
        }
        else if (name == "build_dir") {
            if (!out.build_dir.empty()) {
                payload.collector.collect_issue<AST::Issue::RepeatedManifestAttribute>(
                    payload.context.code_ref(attribute->attribute_id),
                    "'build_dir' is declared twice.");
                return false;
            }

            // **where an artifact may go is BuildLayout's question**, not this reader's - the same one line
            // the `link` and `cc` arms are, so `--build-dir` and a manifest cannot drift on what a build
            // directory is allowed to be
            if (std::optional<std::string> value = reader.string(written)) {
                std::string reason;

                if (!Compiler::resolve_declared_build_dir(
                        value.value(), out.directory, out.build_dir, reason)) {
                    payload.collector.collect_issue<AST::Issue::InvalidAttributeValue>(
                        payload.context.code_ref(attribute->attribute_id), reason);
                    return false;
                }
            }
        }
        else if (name == "cc") {
            // **which member of the spec a scheme fills is CBuild's question**, not this reader's - so a
            // scheme added there does not need a second arm here
            Compiler::apply_cc_attribute(
                written, out.directory, reader, scope != nullptr ? scope->scoped_cc : out.cc);
        }
        else if (const auto tool_name = AST::tool_attribute_name(name)) {
            // a namespace echoc does not own. shape of the value is the tool's job
            Parser::ToolAttribute tool;
            tool.ns = tool_name->first;
            tool.name = tool_name->second;
            tool.value = written;
            out.tools.push_back(std::move(tool));
        }

        // **drained once, after every arm.** A refusal the reader accumulated is what makes an arm's value
        // absent, so a drain per arm is a check the next attribute added here has to remember - and the
        // one it forgets accepts a value the reader already turned down
        Parser::report_attribute_refusals(payload, reader);

        if (payload.collector.has_critical_issues()) {
            return false;
        }
    }

    if (out.name.empty()) {
        payload.collector.collect_issue<AST::Issue::MissingModuleAttribute>(
            payload.context.code_ref(),
            "no '#[module: \"...\"]' - a manifest has to name its module.");
        return false;
    }

    // the name becomes an AST module name, and a **path component**: object, inputs record, C build,
    // `--build-dir` subdirectory. held to the same artifact-name rule a target already is
    if (!is_usable_artifact_name(out.name)) {
        payload.collector.collect_issue<AST::Issue::UnusableModuleName>(
            payload.context.code_ref(),
            fmt::format(
                "'{}' is not a usable module name - letters, digits, '_' and '-' only.", out.name));
        return false;
    }

    // **credited after the loop, not inside it.** The attributes are read in the order they were written
    // and `#[module:]` is only conventionally first, so a `#[link:]` above it would otherwise be attributed
    // to a module with no name - in the one message whose whole job is to say who asked
    for (Compiler::LinkRequirement &requirement : out.link) {
        requirement.declared_by = out.name;
    }

    out.cc.module_name = out.name;

    return true;
}

// **the one glob a manifest's patterns go through**, whichever attribute wrote them.
//
// `noun` is what the refusal calls them, and `exclude` is the one file a pattern may match and not keep.
// Everything else - relative to the manifest and never to the working directory, regular files only,
// deduplicated, and **sorted** - is the same question for both, and the sort is load-bearing twice over: it
// is what keeps a module's token indices, and the link order of its own C objects, a property of the module
// rather than of the order somebody happened to write two patterns in.
//
// **a pattern matching nothing is an error here**, unlike a wildcard on the command line. On the command
// line an empty glob is a pattern the user typed loosely; in a manifest it is a declaration that this module
// is made of files, and finding none of them means the module is silently empty
// `core/*.eco` against `core/array.eco`. one `*` and it cannot cross a slash,
// which is every pattern the stdlib manifest writes
bool match_relative_source_pattern(const std::string &pattern, const std::string &path)
{
    const size_t star = pattern.find('*');
    if (star == std::string::npos) {
        return pattern == path;
    }

    if (pattern.find('*', star + 1) != std::string::npos) {
        return false;
    }

    const std::string prefix = pattern.substr(0, star);
    const std::string suffix = pattern.substr(star + 1);

    if (path.size() < prefix.size() + suffix.size()) {
        return false;
    }

    if (path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }

    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    const std::string mid = path.substr(
        prefix.size(), path.size() - prefix.size() - suffix.size());

    return !mid.empty() && mid.find('/') == std::string::npos;
}

std::filesystem::path virtual_source_path(
    const std::filesystem::path &directory, const std::string &relative)
{
    std::string dir = directory.generic_string();
    while (!dir.empty() && dir.back() == '/') {
        dir.pop_back();
    }

    if (Compiler::is_embedded_stdlib_path(dir)) {
        return Compiler::embedded_stdlib_path(relative);
    }

    return directory / relative;
}

bool expand_manifest_patterns(
    const std::vector<std::string> &patterns,
    const Parser::ModuleManifest &out,
    const std::string &noun,
    const std::filesystem::path &exclude,
    std::vector<std::filesystem::path> &out_files,
    const ManifestReport &into,
    const std::vector<std::string> *virtual_rel = nullptr
)
{
    Compiler::ScopedPhase glob_phase("sources glob");

    std::error_code ec;
    std::set<std::filesystem::path> unique_sources;

    for (const std::string &pattern : patterns) {
        size_t kept = 0;

        if (virtual_rel != nullptr) {
            for (const std::string &rel : *virtual_rel) {
                if (!match_relative_source_pattern(pattern, rel)) {
                    continue;
                }

                const std::filesystem::path resolved = virtual_source_path(out.directory, rel);

                if (!exclude.empty() && resolved == exclude) {
                    continue;
                }

                unique_sources.insert(resolved);
                kept++;
            }
        }
        else {
            for (const std::filesystem::path &match :
                    Parser::expand_source_pattern(out.directory / pattern)) {
                if (!std::filesystem::is_regular_file(match, ec)) {
                    continue;
                }

                const std::filesystem::path resolved = Compiler::canonical_or_absolute(match);

                if (!exclude.empty() && resolved == exclude) {
                    continue;
                }

                unique_sources.insert(resolved);
                kept++;
            }
        }

        if (kept == 0) {
            report_at<AST::Issue::EmptySourcePattern>(into, 1, fmt::format(
                "the {} pattern '{}' matched no files.", noun, pattern));
            return false;
        }
    }

    out_files.assign(unique_sources.begin(), unique_sources.end());

    return true;
}

// canonical dependency manifests, from the paths as written. An entry may name the manifest or the
// directory holding it - see manifest_at.
//
// takes its destination, because a `#[depends:]` inside a target's scope resolves by exactly this rule and
// lands somewhere else - the split every scoped attribute makes, and the reason none of them needed a
// second reading of what they mean
bool resolve_manifest_depend_paths(
    const std::vector<std::string> &written,
    const Parser::ModuleManifest &out,
    std::vector<std::filesystem::path> &into,
    const ManifestReport &report
)
{
    for (const std::string &spelled : written) {
        const std::filesystem::path target = out.directory / spelled;
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(target);

        if (!resolved.has_value()) {
            report_at<AST::Issue::UnresolvableDependency>(report, 1, fmt::format(
                "the dependency '{}' resolves to '{}', which is neither a manifest nor a directory "
                "holding a 'module.eco'.", spelled, target.string()));
            return false;
        }

        into.push_back(Compiler::canonical_or_absolute(resolved.value()));
    }

    return true;
}

// true when this module is neither the program nor a package already under vendor/:
// its #[requires:] still resolve against the program's package dir, and epm install
// at the program is what fills it. the entry is the invocation's, not vendor.parent,
// because --package-dir can point anywhere
bool requirement_is_on_path_dep(
    const std::filesystem::path &module_dir,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &entry_directory)
{
    if (module_dir.empty() || entry_directory.empty()) {
        return false;
    }

    const auto here = Compiler::canonical_or_absolute(module_dir);
    const auto entry = Compiler::canonical_or_absolute(entry_directory);

    if (here == entry) {
        return false;
    }

    if (!package_dir.empty()) {
        const auto vendor = Compiler::canonical_or_absolute(package_dir);
        const auto rel = here.lexically_relative(vendor);

        if (!rel.empty() && rel != "." && *rel.begin() != "..") {
            return false;
        }
    }

    return true;
}

// each requirement's name against the invocation's package directory, appended to the same
// `depends` vector path dependencies already fill - so the DFS, the cycle report and the cache
// key never learn that a requirement was not a path
bool resolve_manifest_require_paths(
    const std::vector<Parser::ModuleRequirement> &requirements,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &module_dir,
    const std::filesystem::path &entry_directory,
    std::vector<std::filesystem::path> &into,
    const ManifestReport &report
)
{
    for (const Parser::ModuleRequirement &requirement : requirements) {
        const std::optional<std::filesystem::path> resolved =
            Parser::manifest_for_requirement(requirement, package_dir);

        if (!resolved.has_value()) {
            const bool on_path = requirement_is_on_path_dep(
                module_dir, package_dir, entry_directory);
            report_span<AST::Issue::PackageNotVendored>(
                report, requirement.span,
                fmt::format("the package \"{}\" is not in 'vendor/'.", requirement.name),
                on_path);
            return false;
        }

        into.push_back(Compiler::canonical_or_absolute(resolved.value()));
    }

    return true;
}

// the same lookup for a requirement written inside a scope, which **records** a missing package rather than
// refusing it - see Parser::UnresolvedRequirement. What is on disk joins `depends` exactly as above, so the
// graph follows a vendored one and never learns that an absent one was written at all
void resolve_scoped_require_paths(
    const std::vector<Parser::ModuleRequirement> &requirements,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &module_dir,
    const std::filesystem::path &entry_directory,
    std::vector<std::filesystem::path> &into,
    std::vector<Parser::UnresolvedRequirement> &unresolved
)
{
    for (const Parser::ModuleRequirement &requirement : requirements) {
        const std::optional<std::filesystem::path> resolved =
            Parser::manifest_for_requirement(requirement, package_dir);

        if (resolved.has_value()) {
            into.push_back(Compiler::canonical_or_absolute(resolved.value()));
            continue;
        }

        unresolved.push_back(Parser::UnresolvedRequirement{
            requirement.name,
            requirement.span.is_valid() ? requirement.span.line() : 1,
            requirement_is_on_path_dep(module_dir, package_dir, entry_directory)
        });
    }
}

// out.sources, from the patterns as written.
//
// **a manifest is never one of its own sources.** It is an Echo file living in the directory it describes,
// so `#[sources: "*.eco"]` matches it - and compiling it would declare its own attributes into the program
// being built. Excluded here rather than by asking authors to write a pattern that avoids it: the obvious
// pattern is the one that breaks
bool expand_manifest_sources(
    const std::vector<std::string> &patterns,
    Parser::ModuleManifest &out,
    const ManifestReport &into,
    const std::vector<std::string> *virtual_rel)
{
    if (patterns.empty()) {
        report_at<AST::Issue::EmptySourcePattern>(into, 1,
            "no '#[sources: \"...\"]' - a manifest has to say which files the module is made of.");
        return false;
    }

    return expand_manifest_patterns(
        patterns, out, "sources", out.path, out.sources, into, virtual_rel);
}

// spec.sources, through the same expander and the same policy `#[sources:]` uses - so `*` means one thing
// in a manifest and there is no second grammar for a pattern.
//
// **an `include:`, `define:` or `flag:` with no `sources:` is refused**, rather than being a spec that
// describes a build of nothing. Those three only ever mean something to a translation unit, so a manifest
// carrying one and no source has said something it cannot have meant - the rule a `#[sources:]` pattern
// matching no files already follows.
//
// takes the spec and who owns it, because a `#[cc:]` inside a target's scope is the same spec answering the
// same rule - the split every scoped attribute makes, and what keeps the fifth `#[cc:]` scheme from needing
// a second arm here
bool expand_cc_sources(
    Compiler::CBuildSpec &spec,
    const Parser::ModuleManifest &out,
    const std::string &owner,
    const ManifestReport &into
)
{
    if (spec.source_patterns.empty()) {
        if (spec.includes.empty() && spec.defines.empty() && spec.flags.empty()) {
            return true;
        }

        report_at<AST::Issue::EmptySourcePattern>(into, 1, fmt::format(
            "{} has '#[cc: ...]' options but no '#[cc: \"sources:...\"]' to apply them to.", owner));
        return false;
    }

    return expand_manifest_patterns(
        spec.source_patterns, out, "C sources", /*exclude=*/{}, spec.sources, into);
}

// out.targets, with every entry resolved against the manifest and proved to be one of this module's own
// sources.
//
// **after expand_manifest_sources, because that is what there is to check against.** A target's entry is a
// file *of* this module rather than one beside it: its declarations are shared with every other target of
// the module, and only its root becomes the program. An entry the module is not made of would be a program
// the rest of the module cannot see, so it is refused where the line number still is
bool resolve_manifest_targets(
    Parser::ModuleManifest &out,
    const ManifestReport &into,
    const std::vector<std::string> *virtual_rel
)
{
    // a scope's own patterns, through the same expander the module's went through - so `*` means one thing
    // in a manifest wherever it is written, and a pattern matching nothing is the same refusal either way.
    // **edges and scoped link/cc are already on `settled`**: record_written_targets copied them, and
    // settle_manifest_edges is the one walk, shared with a `t_dependencies` read
    const auto expand_scope = [&out, &into, virtual_rel](Parser::ModuleTarget &settled) {
        if (!settled.sources_as_written.empty()
            && !expand_manifest_patterns(
                settled.sources_as_written, out, "sources", out.path, settled.sources, into,
                virtual_rel)) {
            return false;
        }

        for (Compiler::LinkRequirement &requirement : settled.link) {
            requirement.declared_by = out.name;
        }

        settled.cc.module_name = out.name;

        return expand_cc_sources(
            settled.cc, out, fmt::format("target '{}'", settled.name), into);
    };

    for (size_t i = 0; i < out.targets.size(); i++) {
        Parser::ModuleTarget &settled = out.targets[i];

        if (!is_usable_artifact_name(settled.name)) {
            report_at<AST::Issue::UnusableTargetName>(into, settled.line, fmt::format(
                "'{}' is not a usable target name - it becomes the name of a binary, so it may hold "
                "only letters, digits, '_' and '-'.", settled.name));
            return false;
        }

        for (size_t earlier = 0; earlier < i; earlier++) {
            if (out.targets[earlier].name != settled.name) {
                continue;
            }

            report_at<AST::Issue::RepeatedManifestAttribute>(into, settled.line, fmt::format(
                "'{}' is declared twice - two targets of one module cannot share a name, because the "
                "name is the binary.", settled.name));
            return false;
        }

        if (!expand_scope(settled)) {
            return false;
        }

        // **a test target names no file**, so every path it does name is a *selection* - resolved against
        // the manifest like any other, and deliberately not checked against `sources`: a file: filter that
        // matches nothing is a refusal the runner makes, where it can say what there was to choose from
        if (!Parser::target_has_entry(settled.kind)) {
            for (const std::string &file : settled.files_as_written) {
                settled.files.push_back(Compiler::settled_path(out.directory, file));
            }

            continue;
        }

        // through the one owner of "a path written relative to something", as `#[link: object]` and
        // `#[cc: include]` are - a manifest's own directory is the base for everything it declares
        const std::filesystem::path resolved = Compiler::settled_path(out.directory, settled.entry_as_written);

        // one sentence for both "no such file" and "not in `sources`", because the remedy is the same
        // one either way: the pattern has to match the file, or the file has to move under one that does.
        //
        // **its own scope counts as sources**, which is the one place the check widened: a target whose
        // scope declares the files it is made of has named a file the module *does* compile whenever that
        // target is the one being built, which is every build in which the entry means anything
        if (std::find(out.sources.begin(), out.sources.end(), resolved) == out.sources.end()
            && std::find(settled.sources.begin(), settled.sources.end(), resolved)
                == settled.sources.end()) {
            report_at<AST::Issue::TargetEntryNotASource>(into, settled.line, fmt::format(
                "'{}' is target '{}'s entry but is not one of this module's sources - a target's entry "
                "has to be a file the module is made of.", settled.entry_as_written, settled.name));
            return false;
        }

        settled.entry = resolved;
    }

    return true;
}

// every manifest this one names, at file scope and inside any target's scope.
//
// **the graph is target-independent on purpose.** Reachability, the duplicate-name check, the cycle report
// and the emit order are all facts about the project rather than about the program being built, and a walk
// that changed shape with `--target` would report a cycle on one target and not on another. Which of these
// a given program actually *compiles* is a later and separate question, answered by module_contribution_for
std::vector<std::filesystem::path> every_dependency(const Parser::ModuleManifest &manifest)
{
    // **through the one merger, with every scope opened.** A hand-rolled union here would be a third place
    // that knows what a scope contributes, and the one that a fifth scoped attribute is not added to
    std::vector<std::filesystem::path> all;
    Parser::append_active_depends(manifest, Parser::all_targets_active(manifest), all);

    // and every hook's tool's - loaded and ordered like any other module, and compiled into no program
    Parser::append_hook_depends(manifest, all);

    return all;
}

// the body of the read, taking the scratch state so a graph walk can reuse it
void record_written_targets(
    const std::vector<WrittenTarget> &written,
    Parser::ModuleManifest &out)
{
    for (const WrittenTarget &target : written) {
        Parser::ModuleTarget settled;
        settled.name = target.name;
        settled.kind = target.kind;
        settled.install = target.install;
        settled.is_public = target.is_public;
        settled.groups = target.groups;
        settled.has_scope = target.has_scope;
        settled.line = target.line;
        settled.entry_as_written = target.entry;
        settled.files_as_written = target.files;
        settled.sources_as_written = target.scope.scoped_sources;
        settled.depends_as_written = target.scope.scoped_depends;
        settled.requirements = target.scope.scoped_requirements;
        settled.link = target.scope.scoped_link;
        settled.cc = target.scope.scoped_cc;
        out.targets.push_back(std::move(settled));
    }
}

// a scope's edges, the file-scope depends/requires, and the hooks' edges - **one spelling**, so a
// `t_dependencies` read and a full one cannot disagree about the graph. Source expansion and entry
// checks stay on the full path: they need files a hook may still be about to create
bool settle_manifest_edges(
    const std::vector<WrittenTarget> &targets_written,
    const std::vector<WrittenHook> &hooks_written,
    const std::vector<std::string> &depends_written,
    Parser::ModuleManifest &out,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &entry_directory,
    const ManifestReport &into
)
{
    for (size_t i = 0; i < targets_written.size(); i++) {
        if (!resolve_scope_edges(
                targets_written[i].scope, out, package_dir, entry_directory,
                out.targets[i].depends, out.targets[i].unresolved, into)) {
            return false;
        }
    }

    return resolve_manifest_depend_paths(depends_written, out, out.depends, into)
        && resolve_manifest_require_paths(
            out.requirements, package_dir, out.directory, entry_directory, out.depends, into)
        && settle_manifest_hooks(
            hooks_written, out, /*resolve=*/true, package_dir, entry_directory, into);
}

bool expand_settled_manifest(
    Parser::ModuleManifest &out,
    const ManifestReport &into,
    Parser::ManifestScratch &scratch)
{
    const std::vector<std::string> *virtual_rel =
        scratch.virtual_source_pool ? &*scratch.virtual_source_pool : nullptr;

    return expand_manifest_sources(out.sources_as_written, out, into, virtual_rel)
        && resolve_manifest_targets(out, into, virtual_rel)
        && expand_cc_sources(out.cc, out, "this module", into);
}

bool read_manifest_with(
    Parser::ManifestScratch &scratch,
    const std::filesystem::path &path,
    Parser::ModuleManifest &out,
    Parser::ManifestRead read,
    const std::optional<std::string> &content)
{
    AST::Module &module = scratch.fresh_module();
    AST::File &file = module.add_file(path);
    const ManifestReport into{ scratch.bundle.collector, module, &file };

    out = Parser::ModuleManifest{};

    if (content.has_value()) {
        file.set_content(*content);
        out.path = path;
        out.directory = path.parent_path();
        if (out.directory.empty() || out.directory == ".") {
            out.directory = path;
        }
    }
    else {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            report_at<AST::Issue::NoSuchManifest>(into, 1, fmt::format(
                "{}: no such manifest file.", path.string()));
            return false;
        }

        out.path = Compiler::canonical_or_absolute(path);
        out.directory = out.path.parent_path();

        std::string io_error;
        if (!file.read_from_disk(io_error)) {
            report_at<AST::Issue::NoSuchManifest>(into, 1, io_error);
            return false;
        }
    }

    // where this manifest's own issues start. The collector is shared across a graph walk, so "did *this*
    // manifest parse" cannot be asked as has_critical_issues() - that answers for every manifest read so
    // far
    const size_t issues_before = scratch.bundle.collector.issues.size();

    Parser::ModuleParser &parser = scratch.parser;

    // **a mistake in the manifest, not a compiler bug.** TokenException becomes a diagnostic; on Windows
    // an uncaught one is an abort with no message
    std::optional<AST::TokenizedFile> lexed;
    try {
        lexed.emplace(parser.make_tokenized_file(module, file));
    }
    catch (const Lexer::TokenException &e) {
        report_at<AST::Issue::UntokenizableManifest>(into, static_cast<uint32_t>(e.get_line()), e.what());
        return false;
    }

    AST::TokenizedFile &tokenized = *lexed;

    // passes 1 and 2 only. A file-scope attribute is collected by the declaration pass, so the body pass
    // would add nothing but a second copy of every attribute node
    {
        Parser::Payload payload =
            parser.make_parser_payload(tokenized, module, scratch.bundle.collector, Parser::Pass::t_type_names);

        // so parse_attribute leaves the unknown-name diagnostic to read_manifest_attributes below, which
        // knows the closed list and the tool-namespace escape
        payload.is_manifest = true;
        Parser::parse_type_names(payload);
    }

    std::vector<std::string> sources_written;
    std::vector<std::string> depends_written;
    std::vector<WrittenTarget> targets_written;
    std::vector<WrittenHook> hooks_written;

    {
        Parser::Payload payload =
            parser.make_parser_payload(tokenized, module, scratch.bundle.collector, Parser::Pass::t_declarations);

        payload.is_manifest = true;
        Parser::parse_symbols(payload);

        if (scratch.bundle.collector.issues.size() > issues_before
            && scratch.bundle.collector.has_critical_issues()) {
            return false;
        }

        if (!read_manifest_attributes(
                payload, module, scratch.parser.target_facts, out, sources_written, depends_written,
                targets_written, hooks_written)) {
            return false;
        }
    }

    out.sources_as_written = sources_written;
    out.depends_as_written = depends_written;

    record_written_targets(targets_written, out);

    // `-p manifest` stops here: epm reads a module whose packages are not on disk yet, so
    // expanding sources or resolving a `#[depends:]` / `#[requires:]` would be a fatal error
    // about a path the author has not fetched
    if (read == Parser::ManifestRead::t_written) {
        return settle_manifest_hooks(
            hooks_written, out, /*resolve=*/false, scratch.package_dir, scratch.entry_directory, into);
    }

    // the edges and nothing else - see ManifestRead. A missing package is still refused, since the
    // question being asked is precisely where it is. The full read continues from here with source
    // expansion, so a later field on a target scope cannot land in two walks
    if (!settle_manifest_edges(
            targets_written, hooks_written, depends_written, out, scratch.package_dir,
            scratch.entry_directory, into)) {
        return false;
    }

    if (read == Parser::ManifestRead::t_dependencies) {
        return true;
    }

    return expand_settled_manifest(out, into, scratch);
}

};

AST::CodeRef Parser::ManifestScratch::pin(const std::filesystem::path &path, uint32_t line, bool read_source)
{
    AST::Module &module = fresh_module();
    AST::File &file = module.add_file(path);

    if (read_source) {
        std::string unreadable;
        file.read_from_disk(unreadable);
    }

    return AST::CodeRef{ &module, pin_manifest(module, &file, line == 0 ? 1 : line).make_slice() };
}

bool Parser::is_usable_artifact_name(const std::string &name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

bool Parser::resolve_scope_edges(
    const Parser::WrittenScope &written,
    const Parser::ModuleManifest &out,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &entry_directory,
    std::vector<std::filesystem::path> &into_depends,
    std::vector<Parser::UnresolvedRequirement> &into_unresolved,
    const Parser::ManifestReport &into
)
{
    if (!resolve_manifest_depend_paths(written.scoped_depends, out, into_depends, into)) {
        return false;
    }

    resolve_scoped_require_paths(
        written.scoped_requirements, package_dir, out.directory, entry_directory, into_depends, into_unresolved);

    return true;
}

Parser::ModuleContribution Parser::module_contribution_for(
    const Parser::ModuleManifest &manifest,
    const Parser::ActiveTargets &active,
    std::string *out_link_error
)
{
    Parser::ModuleContribution out;

    out.sources = manifest.sources;
    out.depends = manifest.depends;
    out.link = manifest.link;
    out.cc = manifest.cc;

    const auto opened = active.find(manifest.name);

    if (opened == active.end()) {
        return out;
    }

    // **the early exit is the load-bearing half.** A manifest whose targets carry no scope, or none this
    // program activates, has to come back holding exactly what it stated at file scope and an empty
    // `active_targets` - that is what keeps its cache key from moving and its object shared across targets
    for (const Parser::ModuleTarget &target : manifest.targets) {
        if (!target.has_scope || opened->second.find(target.name) == opened->second.end()) {
            continue;
        }

        out.active_targets.push_back(target.name);

        // appended after the module's own and deduplicated against them, so a file matched by both a
        // file-scope pattern and a scoped one is compiled once. Order is the module's, then each scope's
        // in written order - the sort inside one list is the expander's and stays where it was
        for (const std::filesystem::path &source : target.sources) {
            if (std::find(out.sources.begin(), out.sources.end(), source) != out.sources.end()) {
                continue;
            }

            out.sources.push_back(source);
        }

        for (const std::filesystem::path &depend : target.depends) {
            if (std::find(out.depends.begin(), out.depends.end(), depend) == out.depends.end()) {
                out.depends.push_back(depend);
            }
        }

        // **through the one merger**, which is what `--link` and every manifest below this one already go
        // through: a requirement a scope repeats after the module stated it is one library, and a raw
        // append would send the duplicate to the link line and to `runtime_library_of` twice
        std::string link_error;

        if (!Compiler::merge_link_requirements(target.link, out.link, link_error)) {
            if (out_link_error != nullptr && out_link_error->empty()) {
                *out_link_error = link_error;
            }
        }

        out.cc.sources.insert(
            out.cc.sources.end(), target.cc.sources.begin(), target.cc.sources.end());
        out.cc.includes.insert(
            out.cc.includes.end(), target.cc.includes.begin(), target.cc.includes.end());
        out.cc.defines.insert(
            out.cc.defines.end(), target.cc.defines.begin(), target.cc.defines.end());
        out.cc.flags.insert(out.cc.flags.end(), target.cc.flags.begin(), target.cc.flags.end());
    }

    return out;
}

void Parser::append_hook_depends(
    const Parser::ModuleManifest &manifest,
    std::vector<std::filesystem::path> &into
)
{
    for (const Parser::ModuleHook &hook : manifest.hooks) {
        for (const std::filesystem::path &dependency : hook.depends) {
            if (std::find(into.begin(), into.end(), dependency) == into.end()) {
                into.push_back(dependency);
            }
        }
    }
}

void Parser::append_active_depends(
    const Parser::ModuleManifest &manifest,
    const Parser::ActiveTargets &active,
    std::vector<std::filesystem::path> &into
)
{
    const auto add = [&into](const std::filesystem::path &dependency) {
        if (std::find(into.begin(), into.end(), dependency) == into.end()) {
            into.push_back(dependency);
        }
    };

    for (const std::filesystem::path &dependency : manifest.depends) {
        add(dependency);
    }

    const auto opened = active.find(manifest.name);

    if (opened == active.end()) {
        return;
    }

    for (const Parser::ModuleTarget &target : manifest.targets) {
        if (!target.has_scope || opened->second.find(target.name) == opened->second.end()) {
            continue;
        }

        for (const std::filesystem::path &dependency : target.depends) {
            add(dependency);
        }
    }
}

Parser::ActiveTargets Parser::all_targets_active(const Parser::ModuleManifest &manifest)
{
    Parser::ActiveTargets active;

    for (const Parser::ModuleTarget &target : manifest.targets) {
        if (target.has_scope) {
            active[manifest.name].insert(target.name);
        }
    }

    return active;
}

bool Parser::refuse_unresolved_scope_requirements(
    const Parser::ModuleManifest &manifest,
    const Parser::ActiveTargets &active,
    Parser::ManifestScratch &scratch
)
{
    const auto opened = active.find(manifest.name);

    if (opened == active.end()) {
        return true;
    }

    bool clean = true;

    for (const Parser::ModuleTarget &target : manifest.targets) {
        if (opened->second.find(target.name) == opened->second.end()) {
            continue;
        }

        if (!refuse_unresolved_requirements(
                manifest, target.unresolved, fmt::format("target '{}'", target.name), scratch)) {
            clean = false;
        }
    }

    return clean;
}

bool Parser::refuse_unresolved_requirements(
    const Parser::ModuleManifest &manifest,
    const std::vector<Parser::UnresolvedRequirement> &unresolved,
    const std::string &owner,
    Parser::ManifestScratch &scratch
)
{
    for (const Parser::UnresolvedRequirement &requirement : unresolved) {
        scratch.report_at_line<AST::Issue::PackageNotVendored>(
            manifest.path, requirement.line,
            fmt::format(
                "the package \"{}\" is not in 'vendor/', and {} requires it.", requirement.name, owner),
            requirement.on_path_dependency);
    }

    return unresolved.empty();
}

bool Parser::read_module_manifest(
    const std::filesystem::path &path,
    Parser::ManifestScratch &scratch,
    Parser::ModuleManifest &out,
    Parser::ManifestRead read,
    const std::optional<std::string> &content
)
{
    return read_manifest_with(scratch, path, out, read, content);
}

bool Parser::complete_manifest_sources(Parser::ModuleManifest &manifest, Parser::ManifestScratch &scratch)
{
    if (!manifest.sources.empty()) {
        return true;
    }

    AST::Module &module = scratch.fresh_module();
    AST::File &file = module.add_file(manifest.path);
    std::string unreadable;
    file.read_from_disk(unreadable);
    const Parser::ManifestReport into{ scratch.bundle.collector, module, &file };

    return expand_settled_manifest(manifest, into, scratch);
}

bool Parser::resolve_module_graph(
    const std::vector<std::filesystem::path> &roots,
    Parser::ManifestScratch &scratch,
    std::vector<Parser::ModuleManifest> &out,
    Parser::ManifestRead read
)
{
    out.clear();

    std::map<std::filesystem::path, ModuleManifest> loaded;

    // read the whole reachable set first, then order it. Two phases because a cycle has to be reported as a
    // cycle rather than as a stack overflow, and because a diamond must read each manifest once
    std::vector<std::filesystem::path> pending;

    // the roots in the order they were given, kept for the emit order below
    std::vector<std::filesystem::path> root_order;

    for (const std::filesystem::path &root : roots) {
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(root);
        if (!resolved.has_value()) {
            scratch.report<AST::Issue::NoSuchManifest>(
                root,
                1,
                fmt::format(
                    "{}: no such manifest - expected a manifest file or a directory holding a 'module.eco'.",
                    root.string()));
            return false;
        }
        pending.push_back(Compiler::canonical_or_absolute(resolved.value()));
        root_order.push_back(pending.back());
    }

    while (!pending.empty()) {
        const std::filesystem::path next = pending.back();
        pending.pop_back();

        if (loaded.count(next) > 0) {
            continue;
        }

        ModuleManifest manifest;
        if (!read_manifest_with(
                scratch,
                next,
                manifest,
                read,
                std::nullopt
            )) {
            return false;
        }

        for (const std::filesystem::path &dependency : every_dependency(manifest)) {
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
            scratch.report<AST::Issue::DuplicateModuleName>(
                path,
                1,
                fmt::format(
                    "two manifests declare the module name '{}': '{}' and '{}'. Module names must be unique "
                    "within a build.", manifest.name, existing->second.string(), path.string()));
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

                scratch.report<AST::Issue::ModuleDependencyCycle>(
                    path,
                    1,
                    fmt::format(
                        "the module dependencies form a cycle: {}. A module is parsed completely before the "
                        "next one starts, so it can only name symbols from modules parsed before it - which "
                        "makes a cycle unsatisfiable rather than merely unsupported.", chain));
                return false;
            }

            in_progress.push_back(path);

            for (const std::filesystem::path &dependency : every_dependency(loaded.at(path))) {
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
    // walking `loaded`, a path-keyed map, on the theory that a canonical-path order is more
    // reproducible than a command line, is reproducible and also wrong: two modules that do not
    // depend on each other are still ordered relative to each other, because a module can name symbols from
    // any module parsed before it. so the order is part of what a build *means*, and the caller is the only
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
