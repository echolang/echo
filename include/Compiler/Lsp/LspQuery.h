#ifndef LSPQUERY_H
#define LSPQUERY_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspSnapshot.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Compiler
{
    namespace Lsp
    {
        enum class OutlineKind
        {
            t_function,
            t_method,
            t_constructor,
            t_operator,
            t_struct,
            t_class,
            t_interface,
            t_enum,
            t_constant,
            t_namespace,
            t_property
        };

        struct HoverAnswer
        {
            std::string type_description;
            std::optional<std::string> signature;
            AST::Span range;
        };

        struct DefinitionAnswer
        {
            std::filesystem::path path;
            AST::Span range;
        };

        struct OutlineSymbol
        {
            std::string name;
            OutlineKind kind = OutlineKind::t_function;
            AST::Span range;
            AST::Span selection;
            std::vector<OutlineSymbol> children;
        };

        struct WorkspaceSymbol
        {
            std::string name;
            OutlineKind kind = OutlineKind::t_function;
            std::filesystem::path path;
            AST::Span range;
            std::string container;
        };

        struct SignatureHelp
        {
            std::string label;
            std::vector<std::string> parameters;
            uint32_t active_parameter = 0;
        };

        // answers from the last good snapshot. Session compiles; these read
        std::optional<HoverAnswer> hover(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location
        );
        std::optional<DefinitionAnswer> definition(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location
        );
        std::vector<OutlineSymbol> document_symbols(const AST::File &file);
        std::vector<DefinitionAnswer> references(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location,
            bool include_declaration
        );
        std::vector<WorkspaceSymbol> workspace_symbols(
            const Snapshot &snapshot,
            const std::string &query
        );
        std::optional<SignatureHelp> signature_help(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location
        );
    };
};

#endif
