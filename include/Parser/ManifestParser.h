#ifndef MANIFESTPARSER_H
#define MANIFESTPARSER_H

#pragma once

#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/TargetFacts.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Parser
{
    // what one module is made of, read from its manifest.
    //
    // a manifest is an Echo file holding nothing but attributes:
    //
    //     #[module: "mylib"]
    //     #[version: "0.1.0"]
    //     #[depends: "../geom/module.eco"]
    //     #[sources: "src/*.eco"]
    //     #[target: exe { name: "clock", entry: "src/main.eco" }]
    //     #[link: lib "m"]
    //     #[cc: sources "c/*.c"]
    //     #[build_dir: "target"]
    //
    // Written in Echo rather than in a config format of its own, so there is one grammar and one lexer
    // in the project, and so a manifest is highlighted, commented and diffed like the code beside it.
    //
    // It costs nothing to read that way. A file-scope attribute is already collected by the
    // *declaration* pass, so the reader runs the real parser's first two passes over a throwaway module
    // and never touches the bundle being built.
    //
    // repeated `depends` and `sources` accumulate, because AST::AttributeList is already a multimap -
    // and so does a list inside one, because AST::AttributeReader::each reads a lone value as a list of
    // it. The two spellings are one rule and not two arms.
    // what a target produces.
    //
    // a **closed vocabulary, tagged in the grammar** rather than spelled into a field of the record, which
    // is what lets a `lib` or a `test` be added later without the common case growing a word - the same
    // shape, and the same reason, as `#[link: lib "m"]`
    enum class TargetKind
    {
        t_executable
    };

    // one program a module produces.
    //
    // **a target is not a module.** It names one file of this module as the program: that file's root is
    // the C `main`, and every other file of the module contributes its declarations and nothing else -
    // which is exactly what a non-entry module's files already get. An entry in a module of its own could
    // only see the rest of the project through `public`, so a project growing a second binary would have
    // to mark up all of its internals; that is the design this one replaced.
    //
    // consequently the module's source list is *identical* whichever target is being built, and nothing
    // here enters a cache key
    struct ModuleTarget
    {
        std::string name;

        // absolute, and always one of the declaring manifest's own `sources` - a file has to belong to the
        // module for its declarations to be shared with the rest of it
        std::filesystem::path entry;

        TargetKind kind = TargetKind::t_executable;
    };

    struct ModuleManifest
    {
        // the manifest file itself, absolute and canonical - this is the identity two `depends` paths
        // pointing at one module have to agree on
        std::filesystem::path path;

        // the directory the manifest sits in. **every relative path in a manifest resolves against this**,
        // never against the working directory: a manifest has to mean the same thing wherever echoc is run
        // from, or a dependency's own `sources` would break the moment it is depended on from elsewhere
        std::filesystem::path directory;

        std::string name;

        // free-form and recorded only. Nothing resolves or constrains it yet - there is no registry to
        // resolve against - but it is in the cache key, so bumping it does force a rebuild
        std::string version;

        // the expanded file list, sorted. Sorted rather than in pattern order so the module's token indices
        // and therefore its emitted symbols do not depend on how the patterns were written
        std::vector<std::filesystem::path> sources;

        // the manifests this module needs parsed before it, absolute and canonical
        std::vector<std::filesystem::path> depends;

        // the programs this module produces, in the order they were written - empty for a module that
        // declares none, which is every module that existed before targets did and still means "every
        // file root of this module is the program".
        //
        // **not an input to the module's cache key**, the same statement `link` and `build_dir` make below
        // and for a stronger reason: which target is being built changes nothing about this module's
        // sources at all, so no key varies between two targets of one module
        std::vector<ModuleTarget> targets;

        // what this module needs linked, in the order it was written. **not an input to the module's cache
        // key**, and that is a statement rather than an omission: a link requirement changes no object, only
        // the link step - and the executable is never cached. The manifest's own bytes are already folded
        // into the key, so editing one rebuilds this module anyway
        std::vector<Compiler::LinkRequirement> link;

        // the C sources that ship with this module, if any. `sources` inside it is expanded the same way
        // `#[sources:]` is, by the same expander
        Compiler::CBuildSpec cc;

        // where this module's build artifacts go, absolute, and **empty when the manifest declares none** -
        // which is what lets the default and the command line answer instead. Resolved here rather than at
        // the point of use for the reason every other path in this struct is: a manifest has to mean the
        // same thing wherever echoc is run from.
        //
        // **not an input to the module's cache key**, and that is a statement rather than an omission, the
        // same one `link` makes: where an object is put changes nothing about the object. The manifest's
        // own bytes are already folded into the key, so editing this line rebuilds the module anyway
        std::filesystem::path build_dir;
    };

    // the paths one source pattern names, in no particular order and unfiltered - a directory can come back,
    // exactly as it can from a glob. **one owner for "what files does this pattern name"**, asked by the
    // manifest reader and by the wildcards on the command line, so the two cannot answer differently.
    //
    // a `**` component anywhere makes the walk recursive. The two shapes that actually occur -
    // `<fixed dirs>/*[.ext]` and `<fixed dirs>/**/*[.ext]` - are walked with a directory iterator, and
    // anything else falls through to the vendored glob, so the grammar is glob's and only the cost differs:
    // glob::glob builds a std::regex per call, which measured at ~5 ms each in a debug build and made the
    // standard library's three patterns cost nearly twice what lexing all of it does. `--timings` found that.
    std::vector<std::filesystem::path> expand_source_pattern(const std::filesystem::path &pattern);

    // **the sole answer to "what manifest does this path name".** A `-m`, a `#[depends:]` entry and the
    // project discovered in the working directory may each name the manifest file or the directory
    // holding one, because both read naturally and only one of them survives moving the file - so the
    // `module.eco` inside a directory is spelled here and asked for everywhere else. Nullopt when there
    // is no manifest there at all
    std::optional<std::filesystem::path> manifest_at(const std::filesystem::path &target);

    // reads one manifest. `out_error` is a located message on failure - `<file>:<line>: <what>` - in the
    // shape EchoTests::parse_eco_test_file uses, and for the same reason: **anything the format does not
    // understand is an error, never a no-op.**
    //
    // An unknown attribute, a missing or repeated `module`, a `sources` pattern matching nothing, a
    // `depends` path that does not exist. Each of those is a manifest that does not describe what its
    // author meant, and silently compiling something else is worse than refusing.
    //
    // a `depends` entry may name either a manifest file or the directory holding one, in which case
    // `module.eco` inside it is used.
    //
    // `facts` is what a `#[if: ...]` in the manifest is evaluated against, and it is a parameter rather than
    // a host default for the reason the whole feature exists: a manifest may gate its own `#[sources:]`, and
    // a source list chosen for one platform while the files in it are filtered for another is silent. It
    // must be the same facts the module's sources will be parsed with - the invocation's, not the machine's
    bool read_module_manifest(
        const std::filesystem::path &path,
        const Compiler::TargetFacts &facts,
        ModuleManifest &out,
        std::string &out_error);

    // every manifest reachable from `roots`, in the order the modules must be parsed: a dependency before
    // whatever depends on it.
    //
    // the order is not a convenience. Parser::ModuleParser::parse_module runs all three passes over a whole
    // module before the next module starts, so a module can name symbols from an earlier module and not
    // from a later one - the graph *has* to be a DAG, and a cycle is reported rather than broken
    // arbitrarily. Duplicates collapse on the canonical manifest path, so a diamond is fine and a module
    // depended on twice is parsed once.
    bool resolve_module_graph(
        const std::vector<std::filesystem::path> &roots,
        const Compiler::TargetFacts &facts,
        std::vector<ModuleManifest> &out,
        std::string &out_error);
};

#endif
