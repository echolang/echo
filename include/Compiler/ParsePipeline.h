#ifndef PARSEPIPELINE_H
#define PARSEPIPELINE_H

#pragma once

#include "AST/ASTBundle.h"
#include "Compiler/CompilerOptions.h"
#include "Parser/ManifestParser.h"
#include "Parser/ModuleParser.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class Monomorphizer;
};

namespace Compiler
{
    // a tokenization or `#[if:]` filter refusal, with the **exact** title and message
    // `handle_parse` used to render. the path is extra for a language server that wants a
    // whole-file diagnostic at 0:0; the driver ignores it and calls `render_untyped`
    struct FrontEndFailure
    {
        std::string title;
        std::string message;
        std::optional<std::filesystem::path> path;
    };

    // in-memory document overlay. a hit uses the `(path, content)` InputFile constructor;
    // a miss, or an empty function, reads the path from disk. the driver passes nothing
    typedef std::function<std::optional<std::string>(const std::filesystem::path &)> SourceOverride;

    // the inputs parse_front_end_bundle needs, so an eighth positional cannot drift from the
    // seventh. overlay empty means disk
    struct ParseRequest
    {
        const std::vector<const Parser::ModuleManifest *> &manifests;
        const std::vector<std::filesystem::path> &roots;
        const Parser::ActiveTargets &active_targets;
        const std::vector<std::filesystem::path> &loose_sources;
        bool with_stdlib = true;
        SourceOverride overlay;
    };

    // the analysis pipeline between parsing and codegen. ConstantExpander, Monomorphizer,
    // PointerAdjuster, AccessPass, TypeChecker - in that order. `after_monomorphize` sits
    // between Monomorphizer and PointerAdjuster for the one caller that dumps instances
    // there; everyone else leaves it empty
    void run_semantic_pipeline(
        AST::Bundle &bundle,
        const CompilerOptions &options,
        std::function<void(AST::Monomorphizer &)> after_monomorphize = nullptr
    );

    // the project `module.eco` in `directory`, when there is one. the driver asks with CWD;
    // the language server asks with the workspace root
    std::optional<std::filesystem::path> discover_project_manifest(
        const std::filesystem::path &directory
    );

    // the manifests this invocation builds, in the order the modules have to be parsed.
    // refusals sit on `scratch.bundle.collector`; the caller renders them. `scratch` is
    // taken rather than built here so the tokens a refusal names still exist when they
    // are printed
    bool resolve_front_end_manifests(
        const std::vector<std::string> &named_roots,
        bool with_stdlib,
        bool allow_project_discovery,
        const std::filesystem::path &discovery_dir,
        const std::filesystem::path &package_dir_override,
        Parser::ManifestScratch &scratch,
        std::vector<Parser::ModuleManifest> &out,
        std::vector<std::filesystem::path> &out_roots
    );

    // parse the stdlib (when embedded), the manifest modules, then the loose sources as
    // `main`. ProgressStep placement is the driver's, kept here so a lift cannot move a
    // row. a filter or tokenization refusal comes back as FrontEndFailure; success is
    // nullopt even when the collector already holds issues
    std::optional<FrontEndFailure> parse_front_end_bundle(
        const ParseRequest &request,
        AST::Bundle &bundle,
        Parser::ModuleParser &parser
    );

    // does any of these manifests already own the `main` module name, so loose sources
    // have nowhere to go. one predicate for the driver's cerr refusal and the server
    bool manifest_claims_main_module(const std::vector<const Parser::ModuleManifest *> &manifests);
    bool manifest_claims_main_module(const std::vector<Parser::ModuleManifest> &manifests);

    // is this manifest one of the roots the invocation named, as opposed to one pulled in
    // behind it. `resolve_front_end_manifests` already settled each root to the path a
    // manifest records as its own, so this is membership, not another `manifest_at`
    bool manifest_is_a_root(
        const Parser::ModuleManifest &manifest,
        const std::vector<std::filesystem::path> &roots
    );

    // does this manifest name a file of its own as a program? the *executable* targets,
    // not every target: a `#[target: test]` produces no artifact. two readers -
    // resolve_programs and collect_shared_top_level_code
    bool module_declares_a_program(const Parser::ModuleManifest &manifest);

    // every `#[target:]` scope this project declares, so a language server sees every file
    // the project can compile. not one program: the server has no Program, and `--target`
    // is compiling-only. `module_contribution_for` is still the owner of what those scopes
    // contribute; `all_targets_active` is the graph loader's copy of the same map
    Parser::ActiveTargets workspace_analysis_targets(
        const std::vector<Parser::ModuleManifest> &manifests
    );
};

#endif
