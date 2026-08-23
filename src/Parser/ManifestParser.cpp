#include "Parser/ManifestParser.h"

#include "Compiler/BuildLayout.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/SettledPath.h"

#include "Parser/AttributeParser.h"
#include "Parser/ManifestPackage.h"
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

namespace
{

// the attribute names a manifest may use are AST::is_known_manifest_attribute's, spelled there beside the
// union the shared attribute parser answers with. Anything else is an error rather than an ignored line,
// which is the one rule the whole format rests on: `#[sources: ...]` misspelled `#[source: ...]` would
// otherwise produce a module with no files and no complaint

// a pin on a manifest file, so a refusal that has no attribute token still has a CodeRef.
// minted: nothing in the source spells this token
TokenReference pin_manifest(AST::Module &module, AST::File *file, uint32_t line)
{
    return module.tokens[module.tokens.push_minted("", Token::Type::t_unknown, line, 1, file)];
}

template <typename Issue>
void report_manifest(
    AST::Collector &collector,
    AST::Module &module,
    AST::File *file,
    uint32_t line,
    std::string message
)
{
    collector.collect_issue<Issue>(
        AST::CodeRef{ &module, pin_manifest(module, file, line).make_slice() },
        std::move(message));
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

struct ManifestReport
{
    AST::Collector &collector;
    AST::Module &module;
    AST::File *file;
};

template <typename Issue>
void report_at(const ManifestReport &into, uint32_t line, std::string message)
{
    report_manifest<Issue>(into.collector, into.module, into.file, line == 0 ? 1 : line, std::move(message));
}

template <typename Issue>
void report_span(const ManifestReport &into, const TokenSpan &span, std::string message)
{
    if (!span.is_valid()) {
        report_at<Issue>(into, 1, std::move(message));
        return;
    }

    into.collector.collect_issue<Issue>(
        AST::CodeRef { &into.module, span.slice() },
        std::move(message));
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

// what one kind's *record* is read against, so the shape rules are stated once per kind rather than
// re-derived at each field. `exe` owes an entry file and a `test` is refused for writing one
struct TargetKindShape
{
    // the fields this kind understands, in the order a refusal lists them
    std::vector<std::string> fields;

    // the fields it cannot do without
    std::vector<std::string> required;

    // what a record-less `#[target: <kind>]` means, or nothing when the kind needs one written
    std::optional<std::string> default_name;
};

// the kinds a target may produce. **one row per spelling**, read through AttributeReader::tag, so a
// misspelled kind refuses in the same words a misspelled `#[link:]` scheme does.
//
// the table is what keeps a second kind from costing a grammar: neither needs the common case to grow a
// word, which is the whole reason the kind is the tag rather than a `kind:` inside the record
const std::vector<std::pair<std::string, Parser::TargetKind>> &target_kind_table()
{
    static const std::vector<std::pair<std::string, Parser::TargetKind>> table = {
        { "exe", Parser::TargetKind::t_executable },
        { "test", Parser::TargetKind::t_test },
    };

    return table;
}

// what each kind's record may say. **one row per kind**, because "which fields are these" is a question
// per kind and answering it inside the field loop is how one kind's rule ends up applied to another's
const TargetKindShape &target_kind_shape(Parser::TargetKind kind)
{
    static const TargetKindShape executable = {
        { "name", "entry" },
        { "name", "entry" },
        std::nullopt
    };

    // **a test target needs no record at all**, and `#[target: test]` is the shape that says so: it runs
    // every test the module has. `groups` and `files` narrow it, and are the same selection `--filter`
    // states - one selection engine, two spellings, the call `#[link:]` and `--link` already make
    static const TargetKindShape test = {
        { "name", "groups", "files" },
        {},
        std::string("tests")
    };

    switch (kind) {
    case Parser::TargetKind::t_executable:
        return executable;
    case Parser::TargetKind::t_test:
        return test;
    }

    return executable;
}

// a target as written, before `#[sources:]` has been expanded to check its entry against
struct WrittenTarget
{
    std::string name;
    std::string entry;
    Parser::TargetKind kind = Parser::TargetKind::t_executable;
    std::vector<std::string> groups;
    std::vector<std::string> files;
    uint32_t line = 0;

    // what this target's `{ ... }` said, in the same as-written shape the four fields above are in. The
    // patterns go through the one expander after the module's own, for the same reason the module's do
    bool has_scope = false;
    std::vector<std::string> scoped_sources;
    std::vector<std::string> scoped_depends;
    std::vector<Parser::ModuleRequirement> scoped_requirements;
    std::vector<Compiler::LinkRequirement> scoped_link;
    Compiler::CBuildSpec scoped_cc;
};

// `#[target: ...]`, in every shape it takes.
//
//     #[target: { name: "clock", entry: "src/main.eco" }]       untagged, which is an executable
//     #[target: exe { name: "clock", entry: "src/main.eco" }]   the same, said out loud
//     #[target: [ { ... }, { ... } ]]                           several
//     #[target: test]                                           a kind with nothing to configure
//     #[target: test { name: "fast", groups: ["unit"] }]         and one narrowed
//
// **the tag is the kind and the record is the fields**, which is the shape `#[link: lib { ... }]` already
// has. It is also why a target's *name* is a string in a field rather than the tag: a bare name means
// itself, and the names that may be spelled bare are the closed vocabularies the compiler knows - which a
// program's name is not.
//
// refuses through `reader` rather than answering, exactly as read_manifest_depends does
void read_manifest_targets(
    const AST::AttributeValue &written,
    AST::AttributeReader &reader,
    uint32_t line,
    std::vector<WrittenTarget> &out_targets)
{
    for (const AST::AttributeValue *entry : AST::AttributeReader::each(written)) {
        Parser::TargetKind kind = Parser::TargetKind::t_executable;

        // **an untagged target is an executable**, so the tag is what a second kind will cost rather than
        // what the first one already does - the rule `#[depends:]`'s bare path follows
        if (entry->has_tag()
            && !reader.tag(*entry, target_kind_table(), "target kind",
                Compiler::scheme_list_of(target_kind_table()), kind)) {
            continue;
        }

        // **a bare name that is a kind is that kind with nothing configured.** `#[target: test]` has no
        // record to hang a tag on, so the name has to be the kind - and reading it here rather than as an
        // untagged value is what keeps it from being taken for a nameless executable. A bare name that is
        // *not* a kind falls through to the record arm, where it is refused as not being one
        if (!entry->has_tag() && entry->is(AST::AttributeValueKind::t_name)) {
            const auto found = std::find_if(
                target_kind_table().begin(), target_kind_table().end(),
                [entry](const auto &row) { return row.first == entry->text; });

            if (found != target_kind_table().end()) {
                const TargetKindShape &shape = target_kind_shape(found->second);

                if (!shape.default_name.has_value()) {
                    reader.refuse(entry->span, fmt::format(
                        "a '{}' target has nothing it can default - write its fields: {}.",
                        entry->text, fmt::join(shape.fields, ", ")));
                    continue;
                }

                WrittenTarget bare;
                bare.name = shape.default_name.value();
                bare.kind = found->second;
                bare.line = entry->span.is_valid() ? entry->span.line() : line;

                out_targets.push_back(std::move(bare));
                continue;
            }
        }

        const TargetKindShape &shape = target_kind_shape(kind);

        for (const AST::AttributeValue *record : AST::AttributeReader::payload(*entry)) {
            if (!reader.record(*record)) {
                continue;
            }

            // a **fixed** record: the keys are ours, so one that is not is refused at the key rather than
            // ignored - the format's "understood or an error, never a no-op" rule, per field. Which keys
            // those are is the *kind's* answer, so a `test` is refused for writing an `entry:` at the word
            // rather than being handed one it has no use for
            bool complete = true;

            for (const std::string &required : shape.required) {
                if (reader.require_field(*record, required) == nullptr) {
                    complete = false;
                }
            }

            reader.reject_unknown_fields(*record, shape.fields);

            if (!complete) {
                continue;
            }

            WrittenTarget target;
            target.kind = kind;
            target.line = record->span.is_valid() ? record->span.line() : line;

            if (const AST::AttributeValue *name = reader.field(*record, "name")) {
                std::optional<std::string> spelled = reader.string(*name);

                if (!spelled.has_value()) {
                    continue;
                }

                target.name = spelled.value();
            }
            else {
                // only a kind with a default can get here - `required` held "name" otherwise
                target.name = shape.default_name.value_or("");
            }

            if (const AST::AttributeValue *entry_file = reader.field(*record, "entry")) {
                std::optional<std::string> spelled = reader.string(*entry_file);

                if (!spelled.has_value()) {
                    continue;
                }

                target.entry = spelled.value();
            }

            // both selections read through AttributeReader::each, so one value and a list of them are the
            // same thing written two ways - the rule every other list-valued field here follows
            const auto read_strings = [&reader, record](
                const std::string &key, std::vector<std::string> &into) {
                const AST::AttributeValue *field = reader.field(*record, key);

                if (field == nullptr) {
                    return;
                }

                for (const AST::AttributeValue *item : AST::AttributeReader::each(*field)) {
                    if (std::optional<std::string> spelled = reader.string(*item)) {
                        into.push_back(spelled.value());
                    }
                }
            };

            read_strings("groups", target.groups);
            read_strings("files", target.files);

            out_targets.push_back(std::move(target));
        }
    }
}

//
// takes the payload rather than the module alone: it reports through the collector, so the values have
// to be read while it is alive
bool read_manifest_attributes(
    Parser::Payload &payload,
    AST::Module &module,
    const Compiler::TargetFacts &facts,
    Parser::ModuleManifest &out,
    std::vector<std::string> &out_sources,
    std::vector<std::string> &out_depends,
    std::vector<WrittenTarget> &out_targets)
{
    // which `WrittenTarget` an attribute's `{ ... }` fills. **The arena order is what makes one pass
    // enough**: an owner is parsed before anything written inside it, so its slot is always registered by
    // the time a child asks for it
    std::unordered_map<const AST::AttributeNode *, size_t> scope_of;

    // read from the arena rather than from the file root: the root is only built by the body pass, and the
    // arena preserves the order the attributes were written in
    for (AST::AttributeNode *attribute : module.nodes.of_type<AST::AttributeNode>()) {
        const std::string name = attribute->attribute_id.value();
        const uint32_t line = attribute->attribute_id.line();

        if (!AST::is_known_manifest_attribute(name)) {
            if (AST::is_reserved_manifest_namespace(name)) {
                payload.collector.collect_issue<AST::Issue::ReservedManifestAttribute>(
                    payload.context.code_ref(attribute->attribute_id),
                    "the 'echoc::' namespace is reserved for the compiler.");
            }
            else {
                payload.collector.collect_issue<AST::Issue::UnknownManifestAttribute>(
                    payload.context.code_ref(attribute->attribute_id),
                    fmt::format(
                        "unknown manifest attribute '{}', expected one of: {}",
                        name, AST::known_manifest_attribute_list()));
            }
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
                    "'{}' cannot carry a '{{ ... }}' scope - only a '#[target: ...]' can, a scope being "
                    "what one target says for itself.", name));
            return false;
        }

        // where this attribute's value goes: the module, or the target whose scope it was written in
        std::optional<size_t> scope;

        if (attribute->scope_owner != nullptr) {
            if (scoping != AST::AttributeScoping::t_scopable) {
                payload.collector.collect_issue<AST::Issue::InvalidManifestScope>(
                    payload.context.code_ref(attribute->attribute_id),
                    scoping == AST::AttributeScoping::t_opens_a_scope
                        ? fmt::format(
                            "'{}' cannot be written inside a '{{ ... }}' scope - a scope is one target "
                            "speaking for itself, so it holds no targets of its own.", name)
                        : fmt::format(
                            "'{}' describes the module, not one of its targets - write it at file scope.",
                            name));
                return false;
            }

            // `.at()` rather than a lookup with a refusal behind it: the owner opened a scope, and an
            // attribute that opens one either registers a slot below or returns. That is an invariant of
            // this walk, so it fails as one rather than as a sentence about somebody's manifest
            scope = scope_of.at(attribute->scope_owner);
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
            read_manifest_depends(
                written, reader,
                scope.has_value() ? out_targets[scope.value()].scoped_depends : out_depends);
        }
        else if (name == "requires") {
            Parser::read_manifest_requires(
                written, reader,
                scope.has_value()
                    ? out_targets[scope.value()].scoped_requirements
                    : out.requirements);
        }
        else if (name == "sources") {
            std::vector<std::string> &into =
                scope.has_value() ? out_targets[scope.value()].scoped_sources : out_sources;

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

            // **a scope belongs to one target.** `#[target: [ {...}, {...} ]]` is a legal spelling and a
            // scope on it would have to be copied into each, which is a rule about what two targets share
            // that nobody wrote down - so it is refused where the alternative is one line of typing.
            //
            // guarded on the reader having nothing to say, because a target it turned down produced no
            // entry either - and answering that with a sentence about scopes would bury the refusal that
            // is already waiting to be drained
            if (attribute->opens_scope && !reader.has_refusals()) {
                if (out_targets.size() != before + 1) {
                    payload.collector.collect_issue<AST::Issue::InvalidManifestScope>(
                        payload.context.code_ref(attribute->attribute_id),
                        fmt::format(
                            "a '{{ ... }}' scope belongs to one target, and this '#[target: ...]' declares "
                            "{} - write each of them its own.", out_targets.size() - before));
                    return false;
                }

                out_targets[before].has_scope = true;
                scope_of[attribute] = before;
            }
        }
        else if (name == "link") {
            // **the scheme is checked here, against this invocation's facts.** A `framework:` on a linux
            // build is a mistake in the manifest and refusing it needs the platform, which the reader has
            // and nothing downstream does - by link time there is only a word left
            Compiler::parse_link_attribute(
                written, out.directory, facts, out.name, reader,
                scope.has_value() ? out_targets[scope.value()].scoped_link : out.link);
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
                written, out.directory, reader,
                scope.has_value() ? out_targets[scope.value()].scoped_cc : out.cc);
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

    // the name becomes an AST module name, which codegen requires to be unique across the bundle and
    // the mangler never reads - so the only rule it needs is that it is spellable
    if (out.name.find_first_of(" \t\"") != std::string::npos) {
        payload.collector.collect_issue<AST::Issue::UnusableModuleName>(
            payload.context.code_ref(),
            fmt::format("'{}' is not a usable module name - no spaces or quotes.", out.name));
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
bool expand_manifest_patterns(
    const std::vector<std::string> &patterns,
    const Parser::ModuleManifest &out,
    const std::string &noun,
    const std::filesystem::path &exclude,
    std::vector<std::filesystem::path> &out_files,
    const ManifestReport &into
)
{
    Compiler::ScopedPhase glob_phase("sources glob");

    std::error_code ec;
    std::set<std::filesystem::path> unique_sources;

    for (const std::string &pattern : patterns) {
        size_t kept = 0;

        for (const std::filesystem::path &match : Parser::expand_source_pattern(out.directory / pattern)) {
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

// each requirement's name against the invocation's package directory, appended to the same
// `depends` vector path dependencies already fill - so the DFS, the cycle report and the cache
// key never learn that a requirement was not a path
bool resolve_manifest_require_paths(
    const std::vector<Parser::ModuleRequirement> &requirements,
    const std::filesystem::path &package_dir,
    std::vector<std::filesystem::path> &into,
    const ManifestReport &report
)
{
    for (const Parser::ModuleRequirement &requirement : requirements) {
        const std::optional<std::filesystem::path> resolved =
            Parser::manifest_for_requirement(requirement, package_dir);

        if (!resolved.has_value()) {
            report_span<AST::Issue::PackageNotVendored>(report, requirement.span, fmt::format(
                "the package \"{}\" is not in 'vendor/'.", requirement.name));
            return false;
        }

        into.push_back(Compiler::canonical_or_absolute(resolved.value()));
    }

    return true;
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
    const ManifestReport &into)
{
    if (patterns.empty()) {
        report_at<AST::Issue::EmptySourcePattern>(into, 1,
            "no '#[sources: \"...\"]' - a manifest has to say which files the module is made of.");
        return false;
    }

    return expand_manifest_patterns(patterns, out, "sources", out.path, out.sources, into);
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
    const std::vector<WrittenTarget> &written,
    Parser::ModuleManifest &out,
    const std::filesystem::path &package_dir,
    const ManifestReport &into
)
{
    // a scope's own patterns, through the same expander the module's went through - so `*` means one thing
    // in a manifest wherever it is written, and a pattern matching nothing is the same refusal either way
    const auto expand_scope = [&out, &into, &package_dir](
        const WrittenTarget &target, Parser::ModuleTarget &settled) {
        if (!target.scoped_sources.empty()
            && !expand_manifest_patterns(
                target.scoped_sources, out, "sources", out.path, settled.sources, into)) {
            return false;
        }

        if (!resolve_manifest_depend_paths(target.scoped_depends, out, settled.depends, into)) {
            return false;
        }

        if (!resolve_manifest_require_paths(
                target.scoped_requirements, package_dir, settled.depends, into)) {
            return false;
        }

        settled.link = target.scoped_link;
        settled.cc = target.scoped_cc;

        // credited exactly as the module's own are, and here rather than in the reader for the same
        // reason: `#[module:]` is only conventionally the first line
        for (Compiler::LinkRequirement &requirement : settled.link) {
            requirement.declared_by = out.name;
        }

        settled.cc.module_name = out.name;

        return expand_cc_sources(
            settled.cc, out, fmt::format("target '{}'", target.name), into);
    };

    for (const WrittenTarget &target : written) {
        // **the name becomes a file name**, unlike a module's, which only has to be spellable - a target
        // is written into the build directory under exactly this word, and BuildLayout::target_binary
        // joins it there on the strength of this refusal.
        //
        // **an allow-list, because the deny-list a binary name needs is not writable**: `..` holds no
        // character worth forbidding and still climbs out of the build directory, and `.`, `*` and a
        // newline are each a name some shell or some filesystem reads as something other than a word
        if (target.name.empty() || !std::all_of(target.name.begin(), target.name.end(),
                [](unsigned char c) {
                    return std::isalnum(c) != 0 || c == '_' || c == '-';
                })) {
            report_at<AST::Issue::UnusableTargetName>(into, target.line, fmt::format(
                "'{}' is not a usable target name - it becomes the name of a binary, so it may hold "
                "only letters, digits, '_' and '-'.", target.name));
            return false;
        }

        for (const Parser::ModuleTarget &earlier : out.targets) {
            if (earlier.name != target.name) {
                continue;
            }

            report_at<AST::Issue::RepeatedManifestAttribute>(into, target.line, fmt::format(
                "'{}' is declared twice - two targets of one module cannot share a name, because the "
                "name is the binary.", target.name));
            return false;
        }

        Parser::ModuleTarget settled;
        settled.name = target.name;
        settled.kind = target.kind;
        settled.groups = target.groups;
        settled.has_scope = target.has_scope;
        settled.sources_as_written = target.scoped_sources;
        settled.depends_as_written = target.scoped_depends;
        settled.requirements = target.scoped_requirements;

        if (!expand_scope(target, settled)) {
            return false;
        }

        // **a test target names no file**, so every path it does name is a *selection* - resolved against
        // the manifest like any other, and deliberately not checked against `sources`: a file: filter that
        // matches nothing is a refusal the runner makes, where it can say what there was to choose from
        if (target.kind == Parser::TargetKind::t_test) {
            for (const std::string &file : target.files) {
                settled.files.push_back(Compiler::settled_path(out.directory, file));
            }

            out.targets.push_back(std::move(settled));
            continue;
        }

        // through the one owner of "a path written relative to something", as `#[link: object]` and
        // `#[cc: include]` are - a manifest's own directory is the base for everything it declares
        const std::filesystem::path resolved = Compiler::settled_path(out.directory, target.entry);

        // one sentence for both "no such file" and "not in `sources`", because the remedy is the same
        // one either way: the pattern has to match the file, or the file has to move under one that does.
        //
        // **its own scope counts as sources**, which is the one place the check widened: a target whose
        // scope declares the files it is made of has named a file the module *does* compile whenever that
        // target is the one being built, which is every build in which the entry means anything
        if (std::find(out.sources.begin(), out.sources.end(), resolved) == out.sources.end()
            && std::find(settled.sources.begin(), settled.sources.end(), resolved)
                == settled.sources.end()) {
            report_at<AST::Issue::TargetEntryNotASource>(into, target.line, fmt::format(
                "'{}' is target '{}'s entry but is not one of this module's sources - a target's entry "
                "has to be a file the module is made of.", target.entry, target.name));
            return false;
        }

        settled.entry = resolved;

        out.targets.push_back(std::move(settled));
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
        settled.groups = target.groups;
        settled.has_scope = target.has_scope;
        settled.sources_as_written = target.scoped_sources;
        settled.depends_as_written = target.scoped_depends;
        settled.requirements = target.scoped_requirements;
        out.targets.push_back(std::move(settled));
    }
}

bool read_manifest_with(
    Parser::ManifestScratch &scratch,
    const std::filesystem::path &path,
    Parser::ModuleManifest &out,
    Parser::ManifestRead read)
{
    AST::Module &module = scratch.fresh_module();
    AST::File &file = module.add_file(path);
    const ManifestReport into{ scratch.bundle.collector, module, &file };

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        report_at<AST::Issue::NoSuchManifest>(into, 1, fmt::format(
            "{}: no such manifest file.", path.string()));
        return false;
    }

    out = Parser::ModuleManifest{};
    out.path = Compiler::canonical_or_absolute(path);
    out.directory = out.path.parent_path();

    // where this manifest's own issues start. The collector is shared across a graph walk, so "did *this*
    // manifest parse" cannot be asked as has_critical_issues() - that answers for every manifest read so
    // far
    const size_t issues_before = scratch.bundle.collector.issues.size();

    std::string io_error;
    if (!file.read_from_disk(io_error)) {
        report_at<AST::Issue::NoSuchManifest>(into, 1, io_error);
        return false;
    }

    Parser::ModuleParser &parser = scratch.parser;

    AST::TokenizedFile tokenized = parser.make_tokenized_file(module, file);

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
                targets_written)) {
            return false;
        }
    }

    out.sources_as_written = sources_written;
    out.depends_as_written = depends_written;

    // `-p manifest` stops here: epm reads a module whose packages are not on disk yet, so
    // expanding sources or resolving a `#[depends:]` / `#[requires:]` would be a fatal error
    // about a path the author has not fetched
    if (read == Parser::ManifestRead::t_written) {
        record_written_targets(targets_written, out);
        return true;
    }

    // targets after the sources they name, and before everything else - an entry has to be checked
    // against the expanded list, and there is nothing in `cc` or `depends` that it depends on
    return expand_manifest_sources(sources_written, out, into)
        && resolve_manifest_targets(targets_written, out, scratch.package_dir, into)
        && expand_cc_sources(out.cc, out, "this module", into)
        && resolve_manifest_depend_paths(depends_written, out, out.depends, into)
        && resolve_manifest_require_paths(
            out.requirements, scratch.package_dir, out.depends, into);
}

};

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

template <typename Issue>
void Parser::ManifestScratch::report(
    const std::filesystem::path &path, uint32_t line, std::string message)
{
    AST::Module &module = fresh_module();
    AST::File &file = module.add_file(path);
    report_manifest<Issue>(bundle.collector, module, &file, line == 0 ? 1 : line, std::move(message));
}

bool Parser::read_module_manifest(
    const std::filesystem::path &path,
    Parser::ManifestScratch &scratch,
    Parser::ModuleManifest &out,
    Parser::ManifestRead read
)
{
    return read_manifest_with(scratch, path, out, read);
}

bool Parser::resolve_module_graph(
    const std::vector<std::filesystem::path> &roots,
    Parser::ManifestScratch &scratch,
    std::vector<Parser::ModuleManifest> &out
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
        if (!read_manifest_with(scratch, next, manifest, Parser::ManifestRead::t_full)) {
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
