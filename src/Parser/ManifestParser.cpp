#include "Parser/ManifestParser.h"

#include "Compiler/BuildLayout.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/SettledPath.h"

#include "Parser/AttributeParser.h"
#include "Parser/ModuleParser.h"
#include "Parser/SymbolParser.h"

#include "AST/ASTAttributes.h"
#include "AST/ASTBundle.h"
#include "AST/AttributeNode.h"
#include "AST/LiteralValueNode.h"

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

    // the facts come from the caller and there is no default: this parser is a *second* one, beside the one
    // the module's sources are parsed with, and the two have to agree about what platform this is. When it
    // resolved the host's facts for itself, `--target-os linux` read a gated manifest's darwin arm and then
    // compiled those files as linux
    explicit ManifestScratch(const Compiler::TargetFacts &facts) : parser(facts) {}

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
                "git dependencies are not resolved yet - a dependency is a path to a manifest that is "
                "already on disk. Vendor the module and name it with a path.");
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
    std::vector<WrittenTarget> &out_targets,
    std::string &out_error)
{
    // read from the arena rather than from the file root: the root is only built by the body pass, and the
    // arena preserves the order the attributes were written in
    for (AST::AttributeNode *attribute : module.nodes.of_type<AST::AttributeNode>()) {
        const std::string name = attribute->attribute_id.value();
        const uint32_t line = attribute->attribute_id.line();

        if (!AST::is_known_manifest_attribute(name)) {
            out_error = locate(out.path, line, fmt::format(
                "unknown manifest attribute '{}', expected one of: {}", name, AST::known_manifest_attribute_list()));
            return false;
        }

        if (!attribute->value.has_value()) {
            out_error = locate(out.path, line,
                fmt::format("the '{}' attribute needs a value - write '#[{}: ...]'.", name, name));
            return false;
        }

        const AST::AttributeValue &written = attribute->value.value();

        // **the reader accumulates, this loop drains.** A manifest has no diagnostic renderer of its
        // own yet, so every refusal has to come back out as one `<file>:<line>: <what>` sentence - and
        // the span the reader carried is what points the line at the offending value rather than at the
        // attribute it sat in
        AST::AttributeReader reader(name);

        const auto drained = [&]() {
            if (!reader.has_refusals()) {
                return false;
            }

            const AST::AttributeRefusal &first = reader.refusals().front();
            out_error = locate(out.path, first.span.is_valid() ? first.span.line() : line, first.message);
            return true;
        };

        if (name == "module") {
            if (!out.name.empty()) {
                out_error = locate(out.path, line, "'module' is declared twice.");
                return false;
            }

            if (std::optional<std::string> value = reader.string(written)) {
                out.name = value.value();
            }
        }
        else if (name == "version") {
            if (!out.version.empty()) {
                out_error = locate(out.path, line, "'version' is declared twice.");
                return false;
            }

            if (std::optional<std::string> value = reader.string(written)) {
                out.version = value.value();
            }
        }
        else if (name == "depends") {
            read_manifest_depends(written, reader, out_depends);
        }
        else if (name == "sources") {
            for (const AST::AttributeValue *pattern : AST::AttributeReader::each(written)) {
                if (std::optional<std::string> value = reader.string(*pattern)) {
                    out_sources.push_back(value.value());
                }
            }
        }
        else if (name == "target") {
            // checked against `sources` after the patterns are expanded - resolve_manifest_targets, which
            // is the first moment there is anything to check an entry against
            read_manifest_targets(written, reader, line, out_targets);
        }
        else if (name == "link") {
            // **the scheme is checked here, against this invocation's facts.** A `framework:` on a linux
            // build is a mistake in the manifest and refusing it needs the platform, which the reader has
            // and nothing downstream does - by link time there is only a word left
            Compiler::parse_link_attribute(written, out.directory, facts, out.name, reader, out.link);
        }
        else if (name == "build_dir") {
            if (!out.build_dir.empty()) {
                out_error = locate(out.path, line, "'build_dir' is declared twice.");
                return false;
            }

            // **where an artifact may go is BuildLayout's question**, not this reader's - the same one line
            // the `link` and `cc` arms are, so `--build-dir` and a manifest cannot drift on what a build
            // directory is allowed to be
            if (std::optional<std::string> value = reader.string(written)) {
                std::string reason;

                if (!Compiler::resolve_declared_build_dir(
                        value.value(), out.directory, out.build_dir, reason)) {
                    out_error = locate(out.path, line, reason);
                    return false;
                }
            }
        }
        else if (name == "cc") {
            // **which member of the spec a scheme fills is CBuild's question**, not this reader's - so a
            // scheme added there does not need a second arm here
            Compiler::apply_cc_attribute(written, out.directory, reader, out.cc);
        }

        // **drained once, after every arm.** A refusal the reader accumulated is what makes an arm's value
        // absent, so a drain per arm is a check the next attribute added here has to remember - and the
        // one it forgets accepts a value the reader already turned down
        if (drained()) {
            return false;
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
    std::string &out_error
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
            out_error = locate(out.path,
                fmt::format("the {} pattern '{}' matched no files.", noun, pattern));
            return false;
        }
    }

    out_files.assign(unique_sources.begin(), unique_sources.end());

    return true;
}

// out.sources, from the patterns as written.
//
// **a manifest is never one of its own sources.** It is an Echo file living in the directory it describes,
// so `#[sources: "*.eco"]` matches it - and compiling it would declare its own attributes into the program
// being built. Excluded here rather than by asking authors to write a pattern that avoids it: the obvious
// pattern is the one that breaks
bool expand_manifest_sources(
    const std::vector<std::string> &patterns, Parser::ModuleManifest &out, std::string &out_error)
{
    if (patterns.empty()) {
        out_error = locate(out.path,
            "no '#[sources: \"...\"]' - a manifest has to say which files the module is made of.");
        return false;
    }

    return expand_manifest_patterns(patterns, out, "sources", out.path, out.sources, out_error);
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
    std::string &out_error
)
{
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
            out_error = locate(out.path, target.line, fmt::format(
                "'{}' is not a usable target name - it becomes the name of a binary, so it may hold "
                "only letters, digits, '_' and '-'.", target.name));
            return false;
        }

        for (const Parser::ModuleTarget &earlier : out.targets) {
            if (earlier.name != target.name) {
                continue;
            }

            out_error = locate(out.path, target.line, fmt::format(
                "'{}' is declared twice - two targets of one module cannot share a name, because the "
                "name is the binary.", target.name));
            return false;
        }

        Parser::ModuleTarget settled;
        settled.name = target.name;
        settled.kind = target.kind;
        settled.groups = target.groups;

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
        // one either way: the pattern has to match the file, or the file has to move under one that does
        if (std::find(out.sources.begin(), out.sources.end(), resolved) == out.sources.end()) {
            out_error = locate(out.path, target.line, fmt::format(
                "'{}' is target '{}'s entry but is not one of this module's sources - a target's entry "
                "has to be a file the module is made of.", target.entry, target.name));
            return false;
        }

        settled.entry = resolved;

        out.targets.push_back(std::move(settled));
    }

    return true;
}

// out.cc.sources, through the same expander and the same policy `#[sources:]` uses - so `*` means one thing
// in a manifest and there is no second grammar for a pattern.
//
// **an `include:`, `define:` or `flag:` with no `sources:` is refused**, rather than being a spec that
// describes a build of nothing. Those three only ever mean something to a translation unit, so a manifest
// carrying one and no source has said something it cannot have meant - the rule a `#[sources:]` pattern
// matching no files already follows
bool expand_manifest_cc_sources(Parser::ModuleManifest &out, std::string &out_error)
{
    if (out.cc.source_patterns.empty()) {
        if (out.cc.includes.empty() && out.cc.defines.empty() && out.cc.flags.empty()) {
            return true;
        }

        out_error = locate(out.path,
            "this module has '#[cc: ...]' options but no '#[cc: \"sources:...\"]' to apply them to.");
        return false;
    }

    return expand_manifest_patterns(
        out.cc.source_patterns, out, "C sources", /*exclude=*/{}, out.cc.sources, out_error);
}

// out.depends, canonical. An entry may name the manifest or the directory holding it - see manifest_at
bool resolve_manifest_depends(
    const std::vector<std::string> &written, Parser::ModuleManifest &out, std::string &out_error)
{
    for (const std::string &spelled : written) {
        const std::filesystem::path target = out.directory / spelled;
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(target);

        if (!resolved.has_value()) {
            out_error = locate(out.path, fmt::format(
                "the dependency '{}' resolves to '{}', which is neither a manifest nor a directory "
                "holding a 'module.eco'.", spelled, target.string()));
            return false;
        }

        out.depends.push_back(Compiler::canonical_or_absolute(resolved.value()));
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
    out.path = Compiler::canonical_or_absolute(path);
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

        // so parse_attribute leaves the unknown-name diagnostic to read_manifest_attributes below, which
        // knows the four names a manifest actually accepts
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

        std::string reported =
            manifest_parse_errors(scratch.bundle.collector, issues_before, out.path);

        if (!reported.empty()) {
            out_error = std::move(reported);
            return false;
        }

        if (!read_manifest_attributes(
                payload, module, scratch.parser.target_facts, out, sources_written, depends_written,
                targets_written, out_error)) {
            return false;
        }
    }

    // targets after the sources they name, and before everything else - an entry has to be checked
    // against the expanded list, and there is nothing in `cc` or `depends` that it depends on
    return expand_manifest_sources(sources_written, out, out_error)
        && resolve_manifest_targets(targets_written, out, out_error)
        && expand_manifest_cc_sources(out, out_error)
        && resolve_manifest_depends(depends_written, out, out_error);
}

};

bool Parser::read_module_manifest(
    const std::filesystem::path &path,
    const Compiler::TargetFacts &facts,
    Parser::ModuleManifest &out,
    std::string &out_error
)
{
    ManifestScratch scratch(facts);
    return read_manifest_with(scratch, path, out, out_error);
}

bool Parser::resolve_module_graph(
    const std::vector<std::filesystem::path> &roots,
    const Compiler::TargetFacts &facts,
    std::vector<Parser::ModuleManifest> &out,
    std::string &out_error
)
{
    out.clear();

    // one scratch for the whole reachable set - see ManifestScratch
    ManifestScratch scratch(facts);

    std::map<std::filesystem::path, ModuleManifest> loaded;

    // read the whole reachable set first, then order it. Two phases because a cycle has to be reported as a
    // cycle rather than as a stack overflow, and because a diamond must read each manifest once
    std::vector<std::filesystem::path> pending;

    // the roots in the order they were given, kept for the emit order below
    std::vector<std::filesystem::path> root_order;

    for (const std::filesystem::path &root : roots) {
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(root);
        if (!resolved.has_value()) {
            out_error = fmt::format(
                "{}: no such manifest - expected a manifest file or a directory holding a 'module.eco'.",
                root.string());
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
