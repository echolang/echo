#ifndef LSPRENAME_H
#define LSPRENAME_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspSnapshot.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    namespace Lsp
    {
        struct PrepareRenameAnswer
        {
            AST::Span range;
            std::string placeholder;

            // why this name stays put. empty when the rename can go ahead
            std::string refusal;
        };

        struct RenameEdit
        {
            std::filesystem::path path;
            AST::Span range;
            std::string new_text;
        };

        struct RenameAnswer
        {
            std::vector<RenameEdit> edits;
            std::string refusal;
        };

        // rename edits the user's files, so it refuses a name it cannot follow. every answer
        // is from a snapshot that matches the text exactly: the server compiles first. a name in
        // the standard library, a package, a `test` block, or an inactive `#[if:]` region is a
        // refusal that says so. snapshot coordinates in, snapshot coordinates out
        PrepareRenameAnswer prepare_rename(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location,
            const std::filesystem::path &workspace_root
        );

        // `new_name` can carry the `$` of a variable or a property, or leave it off
        RenameAnswer rename(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location,
            const std::string &new_name,
            const std::filesystem::path &workspace_root
        );
    };
};

#endif
