#ifndef MANIFESTPACKAGE_H
#define MANIFESTPACKAGE_H

#pragma once

#include "Parser/ManifestParser.h"
#include "AST/ASTAttributeValue.h"
#include "AST/ASTAttributeReader.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Parser
{
    // how many `/`-separated components a `#[requires:]` name may have. `resolve_package_dir`
    // walks the same number of parents looking for `vendor/`, so the two have to agree
    inline constexpr int k_max_package_name_depth = 8;

    // a `#[requires:]` name becomes `<package dir>/<name>`. `/` is a vendor prefix
    // (`echolang/libcurl` → `vendor/echolang/libcurl`); `.`, `..`, an empty segment
    // and a backslash are what would leave that directory
    bool package_name_is_usable(const std::string &name);

    // `#[requires: "libcurl" { git: "...", version: "^0.1" }]`, in every shape a list of them takes.
    //
    // **shape only** - no semver parse, matching `#[version:]`. `version` and `git` are required in v1;
    // `rev` is optional. the name is the tag, a string so a hyphenated package is spellable
    void read_manifest_requires(
        const AST::AttributeValue &written,
        AST::AttributeReader &reader,
        std::vector<ModuleRequirement> &out_requirements
    );

    // the manifest this requirement names under `package_dir`, or nothing if it is not vendored
    std::optional<std::filesystem::path> manifest_for_requirement(
        const ModuleRequirement &requirement,
        const std::filesystem::path &package_dir
    );
};

#endif
