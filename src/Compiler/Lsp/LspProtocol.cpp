#include "Compiler/Lsp/LspProtocol.h"

#include "Compiler/Lsp/LspUri.h"
#include "Compiler/SettledPath.h"

namespace Compiler
{
    namespace Lsp
    {
        Json parse_rpc(const std::string &text)
        {
            return Json::parse(text, nullptr, false);
        }

        Json range_json(const Range &range)
        {
            return Json{
                { "start", { { "line", range.start.line }, { "character", range.start.character } } },
                { "end", { { "line", range.end.line }, { "character", range.end.character } } }
            };
        }

        Json span_json(const AST::Span &span, bool utf8)
        {
            return range_json(span_to_lsp_range(span, utf8));
        }

        int lsp_severity(AST::IssueSeverity severity)
        {
            switch (severity) {
            case AST::IssueSeverity::Error:
                return 1;
            case AST::IssueSeverity::Warning:
                return 2;
            case AST::IssueSeverity::Info:
                return 3;
            }

            return 1;
        }

        int lsp_symbol_kind(OutlineKind kind)
        {
            switch (kind) {
            case OutlineKind::t_method:
                return 6;
            case OutlineKind::t_constructor:
                return 9;
            case OutlineKind::t_operator:
                return 25;
            case OutlineKind::t_class:
                return 5;
            case OutlineKind::t_interface:
                return 11;
            case OutlineKind::t_enum:
                return 10;
            case OutlineKind::t_struct:
                return 23;
            case OutlineKind::t_constant:
                return 14;
            case OutlineKind::t_namespace:
                return 3;
            case OutlineKind::t_function:
                return 12;
            case OutlineKind::t_property:
                return 7;
            }

            return 12;
        }

        Json outline_json(const OutlineSymbol &symbol, bool utf8)
        {
            Json item = {
                { "name", symbol.name },
                { "kind", lsp_symbol_kind(symbol.kind) },
                { "range", span_json(symbol.range, utf8) },
                { "selectionRange", span_json(symbol.selection, utf8) }
            };

            if (!symbol.children.empty()) {
                Json children = Json::array();
                for (const OutlineSymbol &child : symbol.children) {
                    children.push_back(outline_json(child, utf8));
                }

                item["children"] = std::move(children);
            }

            return item;
        }

        Json location_json(const std::filesystem::path &path, const AST::Span &span, bool utf8)
        {
            return Json{
                { "uri", uri_from_path(path) },
                { "range", span_json(span, utf8) }
            };
        }

        std::string eco_fence(const std::string &body)
        {
            return "```eco\n" + body + "\n```";
        }

        bool wants_utf8(const Json &params)
        {
            if (!params.is_object() || !params.contains("capabilities")) {
                return false;
            }

            const Json &capabilities = params["capabilities"];
            if (!capabilities.is_object() || !capabilities.contains("general")) {
                return false;
            }

            const Json &general = capabilities["general"];
            if (!general.is_object() || !general.contains("positionEncodings")) {
                return false;
            }

            const Json &encodings = general["positionEncodings"];
            if (!encodings.is_array()) {
                return false;
            }

            for (const Json &encoding : encodings) {
                if (encoding.is_string() && encoding.get<std::string>() == "utf-8") {
                    return true;
                }
            }

            return false;
        }

        bool client_capability(const Json &params, std::initializer_list<const char *> path)
        {
            if (!params.is_object() || !params.contains("capabilities")) {
                return false;
            }

            const Json *at = &params["capabilities"];
            for (const char *key : path) {
                if (!at->is_object() || !at->contains(key)) {
                    return false;
                }

                at = &(*at)[key];
            }

            return at->is_boolean() && at->get<bool>();
        }

        std::filesystem::path workspace_root_of(const Json &params)
        {
            if (params.contains("rootUri") && params["rootUri"].is_string()) {
                const std::string uri = params["rootUri"].get<std::string>();
                if (!uri.empty()) {
                    return path_from_uri(uri);
                }
            }

            if (params.contains("rootPath") && params["rootPath"].is_string()) {
                const std::string path = params["rootPath"].get<std::string>();
                if (!path.empty()) {
                    return Compiler::canonical_or_absolute(path);
                }
            }

            std::error_code ec;
            const std::filesystem::path here = std::filesystem::current_path(ec);
            return ec ? std::filesystem::path{} : here;
        }

        Json diagnostic_json(const AST::Diagnostic &diagnostic, bool utf8)
        {
            std::string message = diagnostic.message;
            for (const auto &note : diagnostic.notes) {
                message += "\n" + note.message;
            }

            Json item = {
                { "range", span_json(diagnostic.primary, utf8) },
                { "severity", lsp_severity(diagnostic.severity) },
                { "source", "echoc" },
                { "message", message }
            };

            if (diagnostic.code.has_value()) {
                item["code"] = diagnostic.code.value();
            }

            if (!diagnostic.labels.empty()) {
                Json related = Json::array();
                for (const auto &label : diagnostic.labels) {
                    if (label.span.file == nullptr) {
                        continue;
                    }

                    related.push_back({
                        { "location", {
                            { "uri", uri_from_path(label.span.file->get_path()) },
                            { "range", span_json(label.span, utf8) }
                        } },
                        { "message", label.message }
                    });
                }

                if (!related.empty()) {
                    item["relatedInformation"] = related;
                }
            }

            return item;
        }

        bool document_path(const Json &params, std::filesystem::path &out)
        {
            if (!params.is_object() || !params.contains("textDocument")) {
                return false;
            }

            const Json &doc = params["textDocument"];
            if (!doc.is_object() || !doc.contains("uri") || !doc["uri"].is_string()) {
                return false;
            }

            out = path_from_uri(doc["uri"].get<std::string>());
            return true;
        }

        bool document_position(
            const Json &params,
            std::filesystem::path &out_path,
            Position &out_position
        )
        {
            if (!document_path(params, out_path) || !params.contains("position")
                || !params["position"].is_object()) {
                return false;
            }

            out_position.line = params["position"].value("line", 0u);
            out_position.character = params["position"].value("character", 0u);
            return true;
        }

        Json live_span_json(LiveViews &views, const AST::Span &span, bool utf8)
        {
            if (span.file == nullptr) {
                return span_json(span, utf8);
            }

            const LiveViews::View &view = views.view_of(*span.file);
            if (!view.live.has_value()) {
                return span_json(span, utf8);
            }

            const std::optional<AST::Location> start = view.map.location_to_live(span.start);
            const std::optional<AST::Location> end = view.map.location_to_live(span.end);
            if (!start.has_value() || !end.has_value()) {
                return Json();
            }

            return range_json(Range{
                live_position_of(view.live.value(), start.value(), utf8),
                live_position_of(view.live.value(), end.value(), utf8) });
        }

        Json live_location_json(LiveViews &views, const DefinitionAnswer &hit, bool utf8)
        {
            Json range = live_span_json(views, hit.range, utf8);
            if (range.is_null()) {
                return Json();
            }

            return Json{ { "uri", uri_from_path(hit.path) }, { "range", std::move(range) } };
        }

        Json completion_item_json(
            const CompletionItem &item,
            const Range &insert,
            const Range &replace,
            bool label_details,
            bool insert_replace
        )
        {
            Json out = {
                { "label", item.label },
                { "kind", static_cast<int>(item.kind) },
                { "sortText", item.sort_text },
                { "filterText", item.filter_text },
                { "insertTextFormat", item.is_snippet ? 2 : 1 }
            };

            if (!item.detail.empty()) {
                out["detail"] = item.detail;
            }

            if (label_details && (!item.label_detail.empty() || !item.label_description.empty())) {
                Json details = Json::object();
                if (!item.label_detail.empty()) {
                    details["detail"] = item.label_detail;
                }
                if (!item.label_description.empty()) {
                    details["description"] = item.label_description;
                }
                out["labelDetails"] = std::move(details);
            }

            if (insert_replace) {
                out["textEdit"] = {
                    { "newText", item.insert_text },
                    { "insert", range_json(insert) },
                    { "replace", range_json(replace) }
                };
            }
            else {
                out["textEdit"] = {
                    { "newText", item.insert_text },
                    { "range", range_json(insert) }
                };
            }

            return out;
        }

        Json rpc_result(const Json &id, Json result)
        {
            return Json{
                { "jsonrpc", "2.0" },
                { "id", id },
                { "result", std::move(result) }
            };
        }

        Json rpc_error(const Json &id, int code, const std::string &message)
        {
            return Json{
                { "jsonrpc", "2.0" },
                { "id", id },
                { "error", { { "code", code }, { "message", message } } }
            };
        }

        Json rpc_notify(const std::string &method, Json params)
        {
            return Json{
                { "jsonrpc", "2.0" },
                { "method", method },
                { "params", std::move(params) }
            };
        }

        Json rpc_request(const Json &id, const std::string &method, Json params)
        {
            Json message = {
                { "jsonrpc", "2.0" },
                { "id", id },
                { "method", method }
            };
            if (!params.is_null()) {
                message["params"] = std::move(params);
            }

            return message;
        }

        Json server_capabilities_json(bool utf8, bool prepare_rename)
        {
            return Json{
                { "textDocumentSync", 1 },
                { "hoverProvider", true },
                { "definitionProvider", true },
                { "documentSymbolProvider", true },
                { "referencesProvider", true },
                { "workspaceSymbolProvider", true },
                { "signatureHelpProvider", { { "triggerCharacters", Json::array({ "(", "," }) } } },
                // the analyzer decides what each of these started: `>` only as `->`, `:` only as
                // `::`, `.` only as a shorthand, `[` only after `#`. anything else answers an empty
                // list
                { "completionProvider", {
                    { "triggerCharacters", Json::array({ "$", ">", ":", ".", "[" }) },
                    { "resolveProvider", false }
                } },
                { "documentHighlightProvider", true },
                { "renameProvider", prepare_rename ? Json{ { "prepareProvider", true } } : Json(true) },
                { "inlayHintProvider", { { "resolveProvider", false } } },
                { "positionEncoding", utf8 ? "utf-8" : "utf-16" },
                // the client watches the disk and sends workspace/didChangeWatchedFiles. a client
                // that sees no flag here is talking to an echoc that ignores them, and has to
                // restart it
                { "experimental", { { "echoWatchedFiles", true } } }
            };
        }

        Json workspace_symbol_json(const WorkspaceSymbol &symbol, bool utf8)
        {
            Json item = {
                { "name", symbol.name },
                { "kind", lsp_symbol_kind(symbol.kind) },
                { "location", location_json(symbol.path, symbol.range, utf8) }
            };
            if (!symbol.container.empty()) {
                item["containerName"] = symbol.container;
            }

            return item;
        }

        Json signature_help_json(const SignatureHelp &help)
        {
            Json parameters = Json::array();
            for (const std::string &parameter : help.parameters) {
                parameters.push_back({ { "label", parameter } });
            }

            return Json{
                { "signatures", Json::array({
                    {
                        { "label", help.label },
                        { "parameters", std::move(parameters) }
                    }
                }) },
                { "activeSignature", 0 },
                { "activeParameter", help.active_parameter }
            };
        }

        Json inlay_hint_json(const Position &at, const InlayHint &hint)
        {
            return Json{
                { "position", { { "line", at.line }, { "character", at.character } } },
                { "label", hint.label },
                { "kind", 1 },
                { "paddingRight", true },
                { "tooltip", hint.tooltip }
            };
        }

        Json workspace_edit_json(
            const std::map<std::string, Json> &edits_by_uri,
            const std::map<std::string, std::optional<int>> &versions,
            bool document_changes
        )
        {
            Json result = Json::object();
            if (document_changes) {
                Json changes = Json::array();
                for (const auto &[uri, edits] : edits_by_uri) {
                    const auto version = versions.find(uri);
                    changes.push_back(Json{
                        { "textDocument", {
                            { "uri", uri },
                            { "version", version != versions.end() && version->second.has_value()
                                ? Json(version->second.value())
                                : Json() }
                        } },
                        { "edits", edits }
                    });
                }
                result["documentChanges"] = std::move(changes);
            }
            else {
                Json changes = Json::object();
                for (const auto &[uri, edits] : edits_by_uri) {
                    changes[uri] = edits;
                }
                result["changes"] = std::move(changes);
            }

            return result;
        }
    };
};
