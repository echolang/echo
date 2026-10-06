#ifndef LSPPROTOCOL_H
#define LSPPROTOCOL_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspCompletion.h"
#include "Compiler/Lsp/LspHints.h"
#include "Compiler/Lsp/LspLiveText.h"
#include "Compiler/Lsp/LspPosition.h"
#include "Compiler/Lsp/LspQuery.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>

namespace Compiler
{
    namespace Lsp
    {
        typedef nlohmann::json Json;

        Json parse_rpc(const std::string &text);
        Json range_json(const Range &range);
        Json span_json(const AST::Span &span, bool utf8);
        int lsp_severity(AST::IssueSeverity severity);
        int lsp_symbol_kind(OutlineKind kind);
        Json outline_json(const OutlineSymbol &symbol, bool utf8);
        Json location_json(const std::filesystem::path &path, const AST::Span &span, bool utf8);
        std::string eco_fence(const std::string &body);
        bool wants_utf8(const Json &params);
        bool client_capability(const Json &params, std::initializer_list<const char *> path);
        std::filesystem::path workspace_root_of(const Json &params);
        Json diagnostic_json(const AST::Diagnostic &diagnostic, bool utf8);
        bool document_path(const Json &params, std::filesystem::path &out);
        bool document_position(
            const Json &params,
            std::filesystem::path &out_path,
            Position &out_position
        );

        Json live_span_json(LiveViews &views, const AST::Span &span, bool utf8);
        Json live_location_json(LiveViews &views, const DefinitionAnswer &hit, bool utf8);

        Json completion_item_json(
            const CompletionItem &item,
            const Range &insert,
            const Range &replace,
            bool label_details,
            bool insert_replace
        );

        Json rpc_result(const Json &id, Json result);
        Json rpc_error(const Json &id, int code, const std::string &message);
        Json rpc_notify(const std::string &method, Json params);
        Json rpc_request(const Json &id, const std::string &method, Json params);

        Json server_capabilities_json(bool utf8, bool prepare_rename);
        Json workspace_symbol_json(const WorkspaceSymbol &symbol, bool utf8);
        Json signature_help_json(const SignatureHelp &help);
        Json inlay_hint_json(const Position &at, const InlayHint &hint);
        Json workspace_edit_json(
            const std::map<std::string, Json> &edits_by_uri,
            const std::map<std::string, std::optional<int>> &versions,
            bool document_changes
        );
    };
};

#endif
