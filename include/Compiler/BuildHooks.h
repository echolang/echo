#ifndef BUILDHOOKS_H
#define BUILDHOOKS_H

#pragma once

#include "AST/ASTDiagnosticRenderer.h"
#include "Compiler/DriverOptions.h"
#include "Compiler/TargetFacts.h"
#include "Parser/ManifestParser.h"

#include <filesystem>
#include <vector>

namespace Compiler
{
    // the root's `#[hook: before_build]`s, after the graph's edges are loaded and before sources
    // expand, so a hook can write the files a `#[sources:]` pattern has to match. `manifests` and
    // `user_roots` are the t_dependencies load: the root is already in memory, so this does not
    // rediscover the project. only the root's hooks run. skipped for loose sources or `--no-hooks`.
    // several user roots that declare a hook are a refusal
    bool run_before_build_hooks(
        const DriverOptions &driver,
        const AST::DiagnosticRenderer &diagnostics,
        const TargetFacts &facts,
        const std::vector<Parser::ModuleManifest> &manifests,
        const std::vector<std::filesystem::path> &user_roots
    );
};

#endif

