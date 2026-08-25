#include "Compiler/Lsp/LspServer.h"

#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspPosition.h"
#include "Compiler/Lsp/LspQuery.h"
#include "Compiler/Lsp/LspSession.h"
#include "Compiler/Lsp/LspTransport.h"
#include "Compiler/Lsp/LspUri.h"
#include "Compiler/SettledPath.h"
#include "eco.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_set>
#include <utility>

typedef nlohmann::json json;

namespace
{
    json parse_json(const std::string &text)
    {
        return json::parse(text, nullptr, false);
    }

    json range_json(const Compiler::Lsp::Range &range)
    {
        return json{
            { "start", { { "line", range.start.line }, { "character", range.start.character } } },
            { "end", { { "line", range.end.line }, { "character", range.end.character } } }
        };
    }

    json span_json(const AST::Span &span, bool utf8)
    {
        return range_json(Compiler::Lsp::span_to_lsp_range(span, utf8));
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

    int lsp_symbol_kind(Compiler::Lsp::OutlineKind kind)
    {
        switch (kind) {
        case Compiler::Lsp::OutlineKind::t_method:
            return 6;
        case Compiler::Lsp::OutlineKind::t_constructor:
            return 9;
        case Compiler::Lsp::OutlineKind::t_operator:
            return 25;
        case Compiler::Lsp::OutlineKind::t_class:
            return 5;
        case Compiler::Lsp::OutlineKind::t_interface:
            return 11;
        case Compiler::Lsp::OutlineKind::t_enum:
            return 10;
        case Compiler::Lsp::OutlineKind::t_struct:
            return 23;
        case Compiler::Lsp::OutlineKind::t_constant:
            return 14;
        case Compiler::Lsp::OutlineKind::t_namespace:
            return 3;
        case Compiler::Lsp::OutlineKind::t_function:
            return 12;
        case Compiler::Lsp::OutlineKind::t_property:
            return 7;
        }

        return 12;
    }

    json outline_json(const Compiler::Lsp::OutlineSymbol &symbol, bool utf8)
    {
        json item = {
            { "name", symbol.name },
            { "kind", lsp_symbol_kind(symbol.kind) },
            { "range", span_json(symbol.range, utf8) },
            { "selectionRange", span_json(symbol.selection, utf8) }
        };

        if (!symbol.children.empty()) {
            json children = json::array();
            for (const Compiler::Lsp::OutlineSymbol &child : symbol.children) {
                children.push_back(outline_json(child, utf8));
            }

            item["children"] = std::move(children);
        }

        return item;
    }

    json location_json(const std::filesystem::path &path, const AST::Span &span, bool utf8)
    {
        return json{
            { "uri", Compiler::Lsp::uri_from_path(path) },
            { "range", span_json(span, utf8) }
        };
    }

    std::string eco_fence(const std::string &body)
    {
        return "```eco\n" + body + "\n```";
    }

    bool wants_utf8(const json &params)
    {
        if (!params.is_object() || !params.contains("capabilities")) {
            return false;
        }

        const json &capabilities = params["capabilities"];
        if (!capabilities.is_object() || !capabilities.contains("general")) {
            return false;
        }

        const json &general = capabilities["general"];
        if (!general.is_object() || !general.contains("positionEncodings")) {
            return false;
        }

        const json &encodings = general["positionEncodings"];
        if (!encodings.is_array()) {
            return false;
        }

        for (const json &encoding : encodings) {
            if (encoding.is_string() && encoding.get<std::string>() == "utf-8") {
                return true;
            }
        }

        return false;
    }

    std::filesystem::path workspace_root_of(const json &params)
    {
        if (params.contains("rootUri") && params["rootUri"].is_string()) {
            const std::string uri = params["rootUri"].get<std::string>();
            if (!uri.empty()) {
                return Compiler::Lsp::path_from_uri(uri);
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

    json diagnostic_json(const AST::Diagnostic &diagnostic, bool utf8)
    {
        std::string message = diagnostic.message;
        for (const auto &note : diagnostic.notes) {
            message += "\n" + note.message;
        }

        json item = {
            { "range", span_json(diagnostic.primary, utf8) },
            { "severity", lsp_severity(diagnostic.severity) },
            { "source", "echoc" },
            { "message", message }
        };

        if (diagnostic.code.has_value()) {
            item["code"] = diagnostic.code.value();
        }

        if (!diagnostic.labels.empty()) {
            json related = json::array();
            for (const auto &label : diagnostic.labels) {
                if (label.span.file == nullptr) {
                    continue;
                }

                related.push_back({
                    { "location", {
                        { "uri", Compiler::Lsp::uri_from_path(label.span.file->get_path()) },
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

    bool document_path(const json &params, std::filesystem::path &out)
    {
        if (!params.is_object() || !params.contains("textDocument")) {
            return false;
        }

        const json &doc = params["textDocument"];
        if (!doc.is_object() || !doc.contains("uri") || !doc["uri"].is_string()) {
            return false;
        }

        out = Compiler::Lsp::path_from_uri(doc["uri"].get<std::string>());
        return true;
    }

    bool document_position(
        const json &params,
        std::filesystem::path &out_path,
        Compiler::Lsp::Position &out_position
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
};

struct Compiler::Lsp::Server::Impl
{
    Transport transport;
    Session session;
    bool utf8 = false;
    bool initialized = false;
    bool shutdown = false;
    bool exit = false;
    bool debug = false;
    std::unordered_set<std::string> published;

    Impl(std::istream &in, std::ostream &out, const DriverOptions &driver) :
        transport(in, out),
        session(driver)
    {
        const char *flag = std::getenv("ECO_LSP_DEBUG");
        debug = flag != nullptr && flag[0] != '\0' && flag[0] != '0';
    }

    void log(const std::string &line, bool always = false);
    void log_rebuild(const RebuildReport &report);
    bool take_rebuild();
    int run();
    void dispatch(const json &message);
    void reply(const json &id, json result);
    void reply_null(const json &id);
    void reply_error(const json &id, int code, const std::string &message);
    void notify(const std::string &method, json params);
    void flush_session_output();
    void handle_initialize(const json &id, const json &params);
    void handle_initialized(const json &params);
    void handle_shutdown(const json &id, const json &params);
    void handle_exit(const json &params);
    void handle_did_open(const json &params);
    void handle_did_change(const json &params);
    void handle_did_close(const json &params);
    void handle_hover(const json &id, const json &params);
    void handle_definition(const json &id, const json &params);
    void handle_document_symbol(const json &id, const json &params);
    void handle_references(const json &id, const json &params);
    void handle_workspace_symbol(const json &id, const json &params);
    void handle_signature_help(const json &id, const json &params);
};

Compiler::Lsp::Server::Server(
    std::istream &in,
    std::ostream &out,
    const DriverOptions &driver
) :
    _impl(std::make_unique<Impl>(in, out, driver))
{
}

Compiler::Lsp::Server::~Server() = default;

int Compiler::Lsp::Server::run()
{
    return _impl->run();
}

void Compiler::Lsp::Server::Impl::log(const std::string &line, bool always)
{
    if (!always && !debug) {
        return;
    }

    std::cerr << "echoc lsp: " << line << std::endl;
    if (initialized) {
        notify("window/logMessage", json{ { "type", 4 }, { "message", line } });
    }
}

void Compiler::Lsp::Server::Impl::log_rebuild(const RebuildReport &report)
{
    std::ostringstream line;
    line << "rebuild " << (report.reason.empty() ? "sync" : report.reason)
        << (report.failed ? " FAILED" : "")
        << " parse=" << report.parse_ms << "ms"
        << " semantic=" << report.semantic_ms << "ms"
        << " index=" << report.index_ms << "ms"
        << " total=" << report.total_ms << "ms"
        << " modules=" << report.modules
        << " files=" << report.files;
    log(line.str(), true);
}

bool Compiler::Lsp::Server::Impl::take_rebuild()
{
    const std::optional<RebuildReport> report = session.take_finished_rebuild();
    if (!report.has_value()) {
        return false;
    }

    log_rebuild(report.value());
    flush_session_output();
    return true;
}

int Compiler::Lsp::Server::Impl::run()
{
    log("server start" + std::string(debug ? " ECO_LSP_DEBUG=1" : " (rebuild timings always; set ECO_LSP_DEBUG=1 for every request)"), true);

    while (!exit) {
        take_rebuild();

        if (initialized && session.dirty() && !session.rebuild_running()) {
            if (!transport.input_pending(150)) {
                log("starting background rebuild (idle)", true);
                session.start_rebuild("idle");
                continue;
            }
        }

        if (session.rebuild_running() && !transport.input_pending(100)) {
            continue;
        }

        const Transport::Frame frame = transport.read_message();
        if (frame.kind == Transport::FrameKind::t_eof) {
            while (session.rebuild_running()) {
                take_rebuild();
                if (session.rebuild_running()) {
                    transport.input_pending(50);
                }
            }

            take_rebuild();
            break;
        }

        if (frame.kind == Transport::FrameKind::t_invalid) {
            std::cerr << "echoc lsp: discarded a malformed frame" << std::endl;
            continue;
        }

        const json message = parse_json(frame.body);
        if (message.is_discarded() || !message.is_object()) {
            std::cerr << "echoc lsp: discarded a malformed JSON-RPC frame" << std::endl;
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        dispatch(message);
        if (debug) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            log("handled in " + std::to_string(ms) + "ms", false);
        }
    }

    return shutdown ? 0 : 1;
}

void Compiler::Lsp::Server::Impl::dispatch(const json &message)
{
    const std::string method = message.value("method", "");
    const json id = message.contains("id") ? message["id"] : json();
    const bool is_request = message.contains("id");
    const json params = message.contains("params") ? message["params"] : json::object();

    enum class MethodKind
    {
        t_request,
        t_notify,
        t_query
    };

    typedef void (Impl::*RequestHandler)(const json &, const json &);
    typedef void (Impl::*NotifyHandler)(const json &);

    struct Method
    {
        const char *name;
        MethodKind kind;
        RequestHandler request;
        NotifyHandler notify;
    };

    static const Method methods[] = {
        { "initialize", MethodKind::t_request, &Impl::handle_initialize, nullptr },
        { "shutdown", MethodKind::t_request, &Impl::handle_shutdown, nullptr },
        { "initialized", MethodKind::t_notify, nullptr, &Impl::handle_initialized },
        { "exit", MethodKind::t_notify, nullptr, &Impl::handle_exit },
        { "textDocument/didOpen", MethodKind::t_notify, nullptr, &Impl::handle_did_open },
        { "textDocument/didChange", MethodKind::t_notify, nullptr, &Impl::handle_did_change },
        { "textDocument/didClose", MethodKind::t_notify, nullptr, &Impl::handle_did_close },
        { "textDocument/hover", MethodKind::t_query, &Impl::handle_hover, nullptr },
        { "textDocument/definition", MethodKind::t_query, &Impl::handle_definition, nullptr },
        { "textDocument/documentSymbol", MethodKind::t_query, &Impl::handle_document_symbol, nullptr },
        { "textDocument/references", MethodKind::t_query, &Impl::handle_references, nullptr },
        { "workspace/symbol", MethodKind::t_query, &Impl::handle_workspace_symbol, nullptr },
        { "textDocument/signatureHelp", MethodKind::t_query, &Impl::handle_signature_help, nullptr },
    };

    const Method *found = nullptr;
    for (const Method &entry : methods) {
        if (method == entry.name) {
            found = &entry;
            break;
        }
    }

    std::filesystem::path path;
    const bool has_path = document_path(params, path);
    const bool unknown_document = has_path && session.file_of(path) == nullptr;

    if (debug) {
        std::ostringstream line;
        line << method
            << (has_path ? " " + path.string() : "")
            << " dirty=" << (session.dirty() ? 1 : 0)
            << " snapshot=" << (session.has_snapshot() ? 1 : 0)
            << " indexed=" << (unknown_document ? 0 : 1)
            << " rebuilding=" << (session.rebuild_running() ? 1 : 0);
        log(line.str(), false);
    }

    if (found != nullptr && found->kind == MethodKind::t_query && initialized) {
        // queries answer from the last snapshot. rebuild now only when there is no
        // snapshot, or this document is dirty and not in the index yet. a miss after
        // a compile is a path-key problem - log it, do not compile the world again
        if (!session.has_snapshot()) {
            log("blocking rebuild: no snapshot yet", true);
            log_rebuild(session.rebuild());
            flush_session_output();
        }
        else if (unknown_document && session.dirty()) {
            log("blocking rebuild: " + path.string() + " not in snapshot yet", true);
            log_rebuild(session.rebuild());
            flush_session_output();
        }
        else if (unknown_document) {
            std::ostringstream miss;
            miss << "file not in snapshot: " << path.string()
                << " (" << session.indexed_paths().size() << " indexed)";
            log(miss.str(), true);
            if (debug) {
                for (const std::string &indexed : session.indexed_paths()) {
                    log("  indexed " + indexed, false);
                }
            }
        }
    }

    if (found == nullptr) {
        if (is_request) {
            reply_error(id, -32601, "Method not found: " + method);
        }

        return;
    }

    if (found->request != nullptr) {
        (this->*found->request)(id, params);
        return;
    }

    if (found->notify != nullptr) {
        (this->*found->notify)(params);
    }
}

void Compiler::Lsp::Server::Impl::handle_initialized(const json &)
{
    initialized = true;
    log("initialized, compiling workspace", true);
    log_rebuild(session.rebuild());
    flush_session_output();
}

void Compiler::Lsp::Server::Impl::handle_shutdown(const json &id, const json &)
{
    shutdown = true;
    reply_null(id);
}

void Compiler::Lsp::Server::Impl::handle_exit(const json &)
{
    exit = true;
}

void Compiler::Lsp::Server::Impl::reply(const json &id, json result)
{
    json message;
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["result"] = std::move(result);
    transport.write_message(message.dump());
}

void Compiler::Lsp::Server::Impl::reply_null(const json &id)
{
    reply(id, nullptr);
}

void Compiler::Lsp::Server::Impl::reply_error(const json &id, int code, const std::string &message_text)
{
    json message;
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["error"] = { { "code", code }, { "message", message_text } };
    transport.write_message(message.dump());
}

void Compiler::Lsp::Server::Impl::notify(const std::string &method, json params)
{
    json message;
    message["jsonrpc"] = "2.0";
    message["method"] = method;
    message["params"] = std::move(params);
    transport.write_message(message.dump());
}

void Compiler::Lsp::Server::Impl::flush_session_output()
{
    std::map<std::string, json> diagnostics_by_uri;
    std::map<std::string, std::optional<int>> versions;

    auto add_uri = [&](const std::filesystem::path &path) -> json & {
        const std::string uri = uri_from_path(path);
        if (!diagnostics_by_uri.contains(uri)) {
            diagnostics_by_uri[uri] = json::array();
            versions[uri] = session.overlay_version(path);
        }

        return diagnostics_by_uri[uri];
    };

    // keep the last snapshot's squiggles even when this rebuild failed - dropping
    // them leaves hover answering from a tree the editor thinks is clean
    if (session.has_snapshot()) {
        for (const AST::Diagnostic &diagnostic : session.diagnostics()) {
            if (diagnostic.primary.file == nullptr) {
                notify("window/showMessage", json{
                    { "type", lsp_severity(diagnostic.severity) == 1 ? 1 : 2 },
                    { "message", diagnostic.message }
                });
                continue;
            }

            add_uri(diagnostic.primary.file->get_path()).push_back(
                diagnostic_json(diagnostic, utf8));
        }
    }

    const std::optional<FrontEndFailure> &failure = session.parse_failure();
    if (failure.has_value()) {
        if (failure->path.has_value()) {
            json item = {
                { "range", range_json({}) },
                { "severity", 1 },
                { "source", "echoc" },
                { "message", failure->title.empty()
                    ? failure->message
                    : failure->title + ": " + failure->message }
            };
            add_uri(failure->path.value()).push_back(std::move(item));
        }
        else {
            const std::string message = failure->title.empty()
                ? failure->message
                : failure->title + ": " + failure->message;
            notify("window/showMessage", json{ { "type", 1 }, { "message", message } });
        }
    }

    std::unordered_set<std::string> now;
    for (auto &[uri, diagnostics] : diagnostics_by_uri) {
        now.insert(uri);
        json params = { { "uri", uri }, { "diagnostics", diagnostics } };
        if (versions[uri].has_value()) {
            params["version"] = versions[uri].value();
        }

        notify("textDocument/publishDiagnostics", std::move(params));
    }

    for (const std::string &uri : published) {
        if (now.count(uri) == 0) {
            notify("textDocument/publishDiagnostics", json{
                { "uri", uri },
                { "diagnostics", json::array() }
            });
        }
    }

    published = std::move(now);
}

void Compiler::Lsp::Server::Impl::handle_initialize(const json &id, const json &params)
{
    session.set_workspace_root(workspace_root_of(params));
    utf8 = wants_utf8(params);

    json result;
    result["capabilities"] = {
        { "textDocumentSync", 1 },
        { "hoverProvider", true },
        { "definitionProvider", true },
        { "documentSymbolProvider", true },
        { "referencesProvider", true },
        { "workspaceSymbolProvider", true },
        { "signatureHelpProvider", { { "triggerCharacters", json::array({ "(", "," }) } } },
        { "positionEncoding", utf8 ? "utf-8" : "utf-16" }
    };
    result["serverInfo"] = { { "name", "echoc" }, { "version", ECO_VERSION_STRING } };

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_did_open(const json &params)
{
    if (!params.is_object() || !params.contains("textDocument")) {
        return;
    }

    const json &doc = params["textDocument"];
    if (!doc.is_object() || !doc.contains("uri") || !doc["uri"].is_string()
        || !doc.contains("text") || !doc["text"].is_string()) {
        return;
    }

    const std::filesystem::path path = path_from_uri(doc["uri"].get<std::string>());
    log("didOpen " + path.string() + " v" + std::to_string(doc.value("version", 0)), true);
    session.did_open(path, doc.value("version", 0), doc["text"].get<std::string>());
}

void Compiler::Lsp::Server::Impl::handle_did_change(const json &params)
{
    if (!params.is_object() || !params.contains("textDocument") || !params.contains("contentChanges")) {
        return;
    }

    const json &doc = params["textDocument"];
    const json &changes = params["contentChanges"];
    if (!doc.is_object() || !doc.contains("uri") || !doc["uri"].is_string()
        || !changes.is_array() || changes.empty()) {
        return;
    }

    const json &last = changes.back();
    if (!last.contains("text") || !last["text"].is_string()) {
        return;
    }

    session.did_change(
        path_from_uri(doc["uri"].get<std::string>()),
        doc.value("version", 0),
        last["text"].get<std::string>());
}

void Compiler::Lsp::Server::Impl::handle_did_close(const json &params)
{
    std::filesystem::path path;
    if (!document_path(params, path)) {
        return;
    }

    session.did_close(path);
}

void Compiler::Lsp::Server::Impl::handle_hover(const json &id, const json &params)
{
    std::filesystem::path path;
    Position position;
    if (!document_position(params, path, position) || session.snapshot() == nullptr) {
        reply_null(id);
        return;
    }

    const AST::File *file = session.file_of(path);
    if (file == nullptr) {
        reply_null(id);
        return;
    }

    const auto hit = hover(*session.snapshot(), *file, echo_location_of(*file, position, utf8));
    if (!hit.has_value()) {
        reply_null(id);
        return;
    }

    std::string markdown = eco_fence(hit->type_description);
    if (hit->signature.has_value()) {
        markdown = eco_fence(hit->signature.value()) + "\n\n" + markdown;
    }

    json result = { { "contents", { { "kind", "markdown" }, { "value", markdown } } } };
    if (hit->range.file != nullptr) {
        result["range"] = span_json(hit->range, utf8);
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_definition(const json &id, const json &params)
{
    std::filesystem::path path;
    Position position;
    if (!document_position(params, path, position) || session.snapshot() == nullptr) {
        reply_null(id);
        return;
    }

    const AST::File *file = session.file_of(path);
    if (file == nullptr) {
        reply_null(id);
        return;
    }

    const auto hit = definition(*session.snapshot(), *file, echo_location_of(*file, position, utf8));
    if (!hit.has_value()) {
        reply_null(id);
        return;
    }

    reply(id, json{
        { "uri", uri_from_path(hit->path) },
        { "range", span_json(hit->range, utf8) }
    });
}

void Compiler::Lsp::Server::Impl::handle_document_symbol(const json &id, const json &params)
{
    std::filesystem::path path;
    if (!document_path(params, path)) {
        reply(id, json::array());
        return;
    }

    const AST::File *file = session.file_of(path);
    if (file == nullptr) {
        reply(id, json::array());
        return;
    }

    json result = json::array();
    for (const OutlineSymbol &symbol : document_symbols(*file)) {
        result.push_back(outline_json(symbol, utf8));
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_references(const json &id, const json &params)
{
    std::filesystem::path path;
    Position position;
    if (!document_position(params, path, position) || session.snapshot() == nullptr) {
        reply(id, json::array());
        return;
    }

    const AST::File *file = session.file_of(path);
    if (file == nullptr) {
        reply(id, json::array());
        return;
    }

    bool include_declaration = true;
    if (params.contains("context") && params["context"].is_object()) {
        include_declaration = params["context"].value("includeDeclaration", true);
    }

    json result = json::array();
    for (const DefinitionAnswer &hit : references(
            *session.snapshot(), *file, echo_location_of(*file, position, utf8), include_declaration)) {
        result.push_back(location_json(hit.path, hit.range, utf8));
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_workspace_symbol(const json &id, const json &params)
{
    if (session.snapshot() == nullptr) {
        reply(id, json::array());
        return;
    }

    const std::string query = params.is_object() ? params.value("query", "") : "";
    json result = json::array();
    for (const WorkspaceSymbol &symbol : workspace_symbols(*session.snapshot(), query)) {
        json item = {
            { "name", symbol.name },
            { "kind", lsp_symbol_kind(symbol.kind) },
            { "location", location_json(symbol.path, symbol.range, utf8) }
        };
        if (!symbol.container.empty()) {
            item["containerName"] = symbol.container;
        }

        result.push_back(std::move(item));
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_signature_help(const json &id, const json &params)
{
    std::filesystem::path path;
    Position position;
    if (!document_position(params, path, position) || session.snapshot() == nullptr) {
        reply_null(id);
        return;
    }

    const AST::File *file = session.file_of(path);
    if (file == nullptr) {
        reply_null(id);
        return;
    }

    const auto help = signature_help(
        *session.snapshot(), *file, echo_location_of(*file, position, utf8));
    if (!help.has_value()) {
        reply_null(id);
        return;
    }

    json parameters = json::array();
    for (const std::string &parameter : help->parameters) {
        parameters.push_back({ { "label", parameter } });
    }

    json result = {
        { "signatures", json::array({
            {
                { "label", help->label },
                { "parameters", std::move(parameters) }
            }
        }) },
        { "activeSignature", 0 },
        { "activeParameter", help->active_parameter }
    };

    reply(id, std::move(result));
}
