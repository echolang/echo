#ifndef PUBLICTOOLS_H
#define PUBLICTOOLS_H

#pragma once

#include "AST/ASTDiagnosticRenderer.h"
#include "Compiler/TargetFacts.h"
#include "Parser/ManifestParser.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // an `exe` a dependency marked `public: true`. the consumer never compiles it; asking for one
    // builds it as a program of the dependency
    struct PublicTool
    {
        // unique in a build: the qualified spelling
        std::string module;

        std::string target;

        // root of the build that produces this
        std::filesystem::path manifest;

        // where that build writes the exe. same expression resolve_programs uses
        std::filesystem::path binary;

        // `<module>:<target>` always, plus the bare name when unique. epm matches these exactly
        std::vector<std::string> refs;

        // child `echoc build` that produces `binary`. hook runner and epm both run this argv
        std::vector<std::string> build_argv;
    };

    // tools a module's direct dependencies offer, plus the near-misses a refusal names
    struct PublicToolSet
    {
        std::string consumer;

        std::vector<PublicTool> tools;

        // consumer's own exes, and dependencies' exes that are not public. refusals only
        std::vector<std::string> own_targets;
        std::vector<std::string> unoffered;

        std::vector<std::string> dependencies;
    };

    // the read a hook and `-p tools` both make: the consumer's edges, then what those offer.
    // two scratches: hook `#[if:]` is the program's facts, a tool's target is the host's
    struct ToolConsumer
    {
        Parser::ManifestScratch root_scratch;
        Parser::ManifestScratch host_scratch;
        Parser::ModuleManifest root;

        explicit ToolConsumer(const TargetFacts &facts);

        // edges only, so a hook can still write the files `#[sources:]` will match
        bool read(const std::filesystem::path &manifest, const std::filesystem::path &package_dir_override);

        // direct dependencies, host facts: a tool runs on this machine
        bool resolve_tools(const std::filesystem::path &build_dir_flag, PublicToolSet &out);

        void print_issues(const AST::DiagnosticRenderer &diagnostics);
    };

    // the tool `ref` selects, or null with a sentence. one owner: hook runner and epm both ask
    const PublicTool *select_public_tool(
        const PublicToolSet &set,
        const std::string &ref,
        std::string &out_refusal
    );

    // `-p tools` as JSON. absolute paths: the reader is about to run them
    std::string public_tools_as_json(const PublicToolSet &set, const std::filesystem::path &package_dir);
};

#endif
