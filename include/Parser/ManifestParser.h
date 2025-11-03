#ifndef MANIFESTPARSER_H
#define MANIFESTPARSER_H

#pragma once

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
    //
    // written in Echo rather than in a config format of its own so there is one grammar and one lexer in
    // the project, and so a manifest is highlighted, commented and diffed like the code beside it. It costs
    // nothing to read that way: a file-scope attribute is already collected by the *declaration* pass, so
    // the reader runs the real parser's first two passes over a throwaway module and never touches the
    // bundle being built.
    //
    // repeated `depends` and `sources` accumulate, because AST::AttributeList is already a multimap. The
    // alternative - one attribute holding a list - would need a list expression the attribute grammar has
    // no reader for.
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

    // reads one manifest. `out_error` is a located message on failure - `<file>:<line>: <what>` - in the
    // shape EchoTests::parse_eco_test_file uses, and for the same reason: **anything the format does not
    // understand is an error, never a no-op.** An unknown attribute, a missing or repeated `module`, a
    // `sources` pattern matching nothing, a `depends` path that does not exist - each of those is a
    // manifest that does not describe what its author meant, and silently compiling something else is
    // worse than refusing.
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
