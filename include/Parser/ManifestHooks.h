#ifndef MANIFESTHOOKS_H
#define MANIFESTHOOKS_H

#pragma once

#include "AST/ASTAttributeReader.h"
#include "AST/ASTAttributeValue.h"
#include "Parser/ManifestScope.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Parser
{
    // a hook as written, before its scope's paths have been resolved against anything
    struct WrittenHook
    {
        WrittenScope scope;
        HookMoment moment = HookMoment::t_before_build;
        std::string name;
        std::string run;
        std::vector<std::string> args;
        uint32_t line = 0;
    };

    // `#[hook: before_build { run: "restgen", args: [...], name: "api" }]`.
    // the tag is the moment and the record is the fields, the shape `#[target: exe { ... }]` has
    void read_manifest_hooks(
        const AST::AttributeValue &written,
        AST::AttributeReader &reader,
        uint32_t line,
        std::vector<WrittenHook> &out_hooks
    );

    // out.hooks, from the hooks as written - named, and then, when `resolve` says so, given their
    // scope's edges. a hook's scope is its tool's
    bool settle_manifest_hooks(
        const std::vector<WrittenHook> &written,
        ModuleManifest &out,
        bool resolve,
        const std::filesystem::path &package_dir,
        const std::filesystem::path &entry_directory,
        const ManifestReport &into
    );
};

#endif
