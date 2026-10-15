#ifndef MANIFESTSCOPE_H
#define MANIFESTSCOPE_H

#pragma once

#include "AST/ASTBundle.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"
#include "Parser/ManifestParser.h"
#include "Token.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Parser
{
    // a pin on a manifest file, so a refusal that has no attribute token still has a CodeRef.
    // minted: nothing in the source spells this token
    inline TokenReference pin_manifest(AST::Module &module, AST::File *file, uint32_t line)
    {
        return module.tokens[module.tokens.push_minted("", Token::Type::t_unknown, line, 1, file)];
    }

    struct ManifestReport
    {
        AST::Collector &collector;
        AST::Module &module;
        AST::File *file;
    };

    template <typename Issue, typename... Args>
    void report_at(const ManifestReport &into, uint32_t line, Args... args)
    {
        into.collector.collect_issue<Issue>(
            AST::CodeRef{ &into.module, pin_manifest(into.module, into.file, line == 0 ? 1 : line).make_slice() },
            args...);
    }

    // what a `{ ... }` scope said, as written - a target's or a hook's. A hook's only ever holds the
    // edges, AST::manifest_scope_admits refusing the rest before an arm writes here
    struct WrittenScope
    {
        std::vector<std::string> scoped_sources;
        std::vector<std::string> scoped_depends;
        std::vector<ModuleRequirement> scoped_requirements;
        std::vector<Compiler::LinkRequirement> scoped_link;
        Compiler::CBuildSpec scoped_cc;
    };

    // a scope's edges, by the rules a module's own follow. one spelling for every scope and every
    // read level, so a `t_dependencies` read reaches exactly the modules a full one does
    bool resolve_scope_edges(
        const WrittenScope &written,
        const ModuleManifest &out,
        const std::filesystem::path &package_dir,
        const std::filesystem::path &entry_directory,
        std::vector<std::filesystem::path> &into_depends,
        std::vector<UnresolvedRequirement> &into_unresolved,
        const ManifestReport &into
    );

    // the name becomes a file name: a target's binary and a hook's state directory
    bool is_usable_artifact_name(const std::string &name);
};

#endif
