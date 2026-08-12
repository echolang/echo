#ifndef MANIFESTPARSER_H
#define MANIFESTPARSER_H

#pragma once

#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/TargetFacts.h"

#include <filesystem>
#include <map>
#include <set>
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
        t_executable,

        // a named selection of this module's tests, run by `echoc test --target <name>`. **it produces no
        // artifact at all**, which is what makes it unlike the kind above rather than a second flavour of
        // it: there is no entry file, no binary and nothing in the build directory - only `groups` and
        // `files`, which are the same selection `--filter` states on the command line
        t_test
    };

    // one program a module produces.
    //
    // **a target is not a module.** It names one file of this module as the program: that file's root is
    // the C `main`, and every other file of the module contributes its declarations and nothing else -
    // which is exactly what a non-entry module's files already get. An entry in a module of its own could
    // only see the rest of the project through `public`, so a project growing a second binary would have
    // to mark up all of its internals; that is the design this one replaced.
    //
    // A target that carries **no scope** costs the module nothing: its source list is identical whichever
    // target is built and its cache key does not move. A target that carries one is the exception, and
    // `ModuleContribution` below is where the two readings meet
    struct ModuleTarget
    {
        std::string name;

        // absolute, and always one of the declaring manifest's own `sources` - a file has to belong to the
        // module for its declarations to be shared with the rest of it.
        //
        // **empty for a `test` target**, which names no file: it selects among the tests the whole module
        // already has. Refused rather than ignored when one is written, so `entry:` cannot look like it did
        // something
        std::filesystem::path entry;

        TargetKind kind = TargetKind::t_executable;

        // a `test` target's selection: the groups and the files whose tests it runs, empty meaning all of
        // them. Read by Compiler::select_tests, which is the same function `--filter` goes through - a
        // declared target is a saved filter and deliberately not a second selection engine
        std::vector<std::string> groups;
        std::vector<std::filesystem::path> files;

        // what this target's `{ ... }` scope contributed - the four things a manifest also says at file
        // scope, said for one target instead of for the module:
        //
        //     #[target: test] {
        //         #[sources: "tests/*.eco"]
        //         #[depends: "../mocklib"]
        //     }
        //
        // **held on the target rather than merged into the manifest above.** Whether they apply is a
        // question about the program being built, and a manifest is read long before the invocation knows
        // which target that is - merging here would settle it at the wrong moment, in the one direction
        // that compiles. `module_contribution_for` is what settles it, per program
        bool has_scope = false;
        std::vector<std::filesystem::path> sources;
        std::vector<std::filesystem::path> depends;
        std::vector<Compiler::LinkRequirement> link;
        Compiler::CBuildSpec cc;
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

    // **what this module compiles, depends on and links, for one program.**
    //
    // The sole owner of that question. It has three readers - the parser's input list, the module cache
    // key and the shared-top-level-code check - and they are three *readings of one answer* rather than
    // three merges of the same two lists. A merge each would be the drift `TargetFacts::tests` already
    // documents between its own two readers, where disagreement is not merely wrong but unsound: the
    // cache would hand one target the object built for another.
    struct ModuleContribution
    {
        // the module's own files, then every active scope's, deduplicated and in that order
        std::vector<std::filesystem::path> sources;
        std::vector<std::filesystem::path> depends;
        std::vector<Compiler::LinkRequirement> link;
        Compiler::CBuildSpec cc;

        // the scopes that answered, in written order. **Empty is what keeps a module's cache key byte for
        // byte what it was** - a module whose targets carry no scope, or whose scopes this program does
        // not activate, folds nothing new and so still shares one object across every target
        std::vector<std::string> active_targets;
    };

    // which of each module's targets a program opens the scopes of, by module name.
    //
    // **keyed by module because a target name is manifest-local.** Two modules may each call a target
    // `tests`, and a flat set of names would have the entry module's `--target` reach into a dependency
    // and change what that dependency compiles - which is the one thing a consumer cannot see coming
    typedef std::map<std::string, std::set<std::string>> ActiveTargets;

    // resolves the above against the targets this program activates. A module named by nothing in the map,
    // or whose targets carry no scope, comes back stating exactly what its manifest states
    ModuleContribution module_contribution_for(
        const ModuleManifest &manifest,
        const ActiveTargets &active
    );

    // just the dependencies of the above, appended to `into` without duplicating what it holds.
    //
    // **the same rule, without building the four lists a caller was going to throw away.** Two readers ask
    // only this - the reachability walk that decides which modules a program compiles at all, and the graph
    // loader, which passes an "everything is active" map because reachability is a fact about the project
    // rather than about the program
    void append_active_depends(
        const ModuleManifest &manifest,
        const ActiveTargets &active,
        std::vector<std::filesystem::path> &into
    );

    // an `ActiveTargets` opening every scope of every module, for the questions that are about the project
    // rather than about one program: the graph has to load and order a module a scope names whether or not
    // this invocation would compile it, or a cycle would be a cycle only on some targets
    ActiveTargets all_targets_active(const std::vector<ModuleManifest> &manifests);

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
