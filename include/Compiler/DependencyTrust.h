#ifndef DEPENDENCYTRUST_H
#define DEPENDENCYTRUST_H

#pragma once

#include "Parser/ManifestParser.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Compiler
{
    // **is this module trusted with the machine.**
    // a root (its hooks already run) and the stdlib are. everything else is a **dependency**:
    // Echo sources, C sources, includes, and path-valued `#[link:]` stay in its tree.
    // a C translation unit can still `#include` / `#embed` an absolute path; that is clang's,
    // and this jail does not read the preprocessor
    bool module_is_trusted(
        const Parser::ModuleManifest &manifest,
        const std::vector<std::filesystem::path> &roots
    );

    // empty, or the sentence refusing one `#[cc: flag]` a dependency wrote.
    // **a deny-list**: plugins, response files, path-valued flags, outputs. how a library compiles
    // its own C is otherwise the author's
    std::optional<std::string> dependency_cc_flag_refusal(const std::string &flag);

    // the same for one `#[cc: define]`. only the name is checked: argv reaches clang with no shell
    std::optional<std::string> dependency_cc_define_refusal(const std::string &define);

    // a dependency's Echo/C sources, includes, and path-valued `#[link:]`s stay in its tree;
    // flags pass the two refusals. asked of the contribution, because a target scope contributes too
    std::optional<std::string> dependency_contribution_refusal(
        const Parser::ModuleManifest &manifest,
        const Parser::ModuleContribution &contribution
    );
};

#endif
