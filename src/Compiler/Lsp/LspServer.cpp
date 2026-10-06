#include "Compiler/Lsp/LspServer.h"

#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspCompletion.h"
#include "Compiler/Lsp/LspHints.h"
#include "Compiler/Lsp/LspLiveText.h"
#include "Compiler/Lsp/LspPosition.h"
#include "Compiler/Lsp/LspProtocol.h"
#include "Compiler/Lsp/LspQuery.h"
#include "Compiler/Lsp/LspRename.h"
#include "Compiler/Lsp/LspSession.h"
#include "Compiler/Lsp/LspTransport.h"
#include "Compiler/Lsp/LspUri.h"
#include "eco.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>

typedef Compiler::Lsp::Json json;

struct Compiler::Lsp::Server::Impl
{
    Transport transport;
    Session session;
    bool utf8 = false;
    bool initialized = false;
    bool shutdown = false;
    bool exit = false;
    bool debug = false;
    int next_request_id = 1;

    // what the client said it can take, from `initialize`
    bool snippet_support = false;
    bool label_details_support = false;
    bool insert_replace_support = false;
    bool prepare_rename_support = false;
    bool document_changes_support = false;
    bool inlay_refresh_support = false;

    std::unordered_set<std::string> published;
    std::string shown_project_message;

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
    void request(const std::string &method, json params);
    void flush_session_output();
    void handle_initialize(const json &id, const json &params);
    void handle_initialized(const json &params);
    void handle_shutdown(const json &id, const json &params);
    void handle_exit(const json &params);
    void handle_did_open(const json &params);
    void handle_did_change(const json &params);
    void handle_did_close(const json &params);
    void handle_did_change_watched_files(const json &params);
    void handle_hover(const json &id, const json &params);
    void handle_definition(const json &id, const json &params);
    void handle_document_symbol(const json &id, const json &params);
    void handle_references(const json &id, const json &params);
    void handle_workspace_symbol(const json &id, const json &params);
    void handle_signature_help(const json &id, const json &params);
    void handle_document_highlight(const json &id, const json &params);
    void handle_inlay_hint(const json &id, const json &params);
    void handle_completion(const json &id, const json &params);
    void handle_prepare_rename(const json &id, const json &params);
    void handle_rename(const json &id, const json &params);
    const AST::File *rename_file(const json &id, const json &params, std::filesystem::path &path, AST::Location &location);

    struct LocatedQuery
    {
        const AST::File *file;
        std::optional<AST::Location> location;
        LiveViews views;
    };

    std::optional<LocatedQuery> locate_query(const json &params);
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

        const json message = parse_rpc(frame.body);
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
    // a response to a request this server sent (workspace/inlayHint/refresh). it carries an id
    // and no method; answering it with 'method not found' would be a reply to a reply
    if (!message.contains("method")) {
        return;
    }

    const std::string method = message.value("method", "");
    const json id = message.contains("id") ? message["id"] : json();
    const bool is_request = message.contains("id");
    const json params = message.contains("params") ? message["params"] : json::object();

    // t_query answers from the last snapshot and compiles first only when there is none.
    // t_live waits on a compile only for a document no snapshot has seen: it reads the editor's
    // text and whatever snapshot there is, because it runs on every keystroke and the statement
    // being typed is in no snapshot anyway.
    // t_fresh compiles first whenever anything changed, because it edits the user's files and
    // a stale tree would write the wrong ranges
    enum class MethodKind
    {
        t_request,
        t_notify,
        t_query,
        t_live,
        t_fresh
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
        { "workspace/didChangeWatchedFiles", MethodKind::t_notify, nullptr, &Impl::handle_did_change_watched_files },
        { "textDocument/hover", MethodKind::t_query, &Impl::handle_hover, nullptr },
        { "textDocument/definition", MethodKind::t_query, &Impl::handle_definition, nullptr },
        { "textDocument/documentSymbol", MethodKind::t_query, &Impl::handle_document_symbol, nullptr },
        { "textDocument/references", MethodKind::t_query, &Impl::handle_references, nullptr },
        { "workspace/symbol", MethodKind::t_query, &Impl::handle_workspace_symbol, nullptr },
        { "textDocument/signatureHelp", MethodKind::t_query, &Impl::handle_signature_help, nullptr },
        { "textDocument/documentHighlight", MethodKind::t_live, &Impl::handle_document_highlight, nullptr },
        { "textDocument/inlayHint", MethodKind::t_live, &Impl::handle_inlay_hint, nullptr },
        { "textDocument/completion", MethodKind::t_live, &Impl::handle_completion, nullptr },
        { "textDocument/prepareRename", MethodKind::t_fresh, &Impl::handle_prepare_rename, nullptr },
        { "textDocument/rename", MethodKind::t_fresh, &Impl::handle_rename, nullptr },
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

    if (found != nullptr && found->kind == MethodKind::t_fresh && initialized
        && (session.dirty() || !session.has_snapshot())) {
        log("blocking rebuild: " + method + " needs the current text", true);
        log_rebuild(session.rebuild());
        flush_session_output();
    }

    // a document just opened is in no snapshot, and the text alone answers almost nothing about it.
    // the one compile a live request waits for is that one; after it, keystrokes keep going
    if (found != nullptr && found->kind == MethodKind::t_live && initialized && unknown_document
        && session.dirty()) {
        log("blocking rebuild: " + path.string() + " not in snapshot yet", true);
        log_rebuild(session.rebuild());
        flush_session_output();
    }

    if (found != nullptr && found->kind == MethodKind::t_query && initialized) {
        // queries answer from the last snapshot. rebuild now only when there is no
        // snapshot, or this document is dirty and not in the index yet. a miss after
        // a compile is a path-key problem: log it, skip compiling the world again
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
    transport.write_message(rpc_result(id, std::move(result)).dump());
}

void Compiler::Lsp::Server::Impl::reply_null(const json &id)
{
    reply(id, nullptr);
}

void Compiler::Lsp::Server::Impl::reply_error(const json &id, int code, const std::string &message_text)
{
    transport.write_message(rpc_error(id, code, message_text).dump());
}

void Compiler::Lsp::Server::Impl::notify(const std::string &method, json params)
{
    transport.write_message(rpc_notify(method, std::move(params)).dump());
}

void Compiler::Lsp::Server::Impl::request(const std::string &method, json params)
{
    transport.write_message(rpc_request(
        "echoc-" + std::to_string(next_request_id++),
        method,
        std::move(params)
    ).dump());
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

    // keep the last snapshot's squiggles even when this rebuild failed. dropping
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

    // the project's own problems, kept until the project is resolved again. one on a file (a
    // manifest line) is a squiggle there; one with nowhere to point is said once per resolution
    std::string project_message;
    for (const ProjectIssue &issue : session.project_issues()) {
        if (issue.path.has_value()) {
            const uint32_t line = issue.line > 0 ? issue.line - 1 : 0;
            add_uri(issue.path.value()).push_back(json{
                { "range", {
                    { "start", { { "line", line }, { "character", 0 } } },
                    { "end", { { "line", line + 1 }, { "character", 0 } } }
                } },
                { "severity", lsp_severity(issue.severity) },
                { "source", "echoc" },
                { "message", issue.message }
            });
            continue;
        }

        project_message += (project_message.empty() ? "" : "\n") + issue.message;
    }

    if (project_message != shown_project_message) {
        if (!project_message.empty()) {
            notify("window/showMessage", json{ { "type", 1 }, { "message", project_message } });
        }

        shown_project_message = project_message;
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

    // the editor asks for inlay hints when a document changes. without this the hints shown are
    // the previous snapshot's until the next keystroke
    if (initialized && inlay_refresh_support) {
        request("workspace/inlayHint/refresh", json());
    }
}

void Compiler::Lsp::Server::Impl::handle_initialize(const json &id, const json &params)
{
    session.set_workspace_root(workspace_root_of(params));
    utf8 = wants_utf8(params);
    snippet_support = client_capability(params, { "textDocument", "completion", "completionItem", "snippetSupport" });
    label_details_support = client_capability(params, { "textDocument", "completion", "completionItem", "labelDetailsSupport" });
    insert_replace_support = client_capability(params, { "textDocument", "completion", "completionItem", "insertReplaceSupport" });
    prepare_rename_support = client_capability(params, { "textDocument", "rename", "prepareSupport" });
    document_changes_support = client_capability(params, { "workspace", "workspaceEdit", "documentChanges" });
    inlay_refresh_support = client_capability(params, { "workspace", "inlayHint", "refreshSupport" });

    json result;
    result["capabilities"] = server_capabilities_json(utf8, prepare_rename_support);
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

void Compiler::Lsp::Server::Impl::handle_did_change_watched_files(const json &params)
{
    if (!params.is_object() || !params.contains("changes") || !params["changes"].is_array()) {
        return;
    }

    std::vector<WatchedFile> files;
    for (const json &change : params["changes"]) {
        if (!change.is_object() || !change.contains("uri") || !change["uri"].is_string()) {
            continue;
        }

        const int type = change.value("type", 2);
        if (type < 1 || type > 3) {
            continue;
        }

        files.push_back(WatchedFile{
            path_from_uri(change["uri"].get<std::string>()),
            static_cast<WatchedChange>(type) });
    }

    log("didChangeWatchedFiles " + std::to_string(files.size()) + " file(s)", true);
    session.did_change_watched(files);
}

std::optional<Compiler::Lsp::Server::Impl::LocatedQuery> Compiler::Lsp::Server::Impl::locate_query(
    const json &params
)
{
    std::filesystem::path path;
    Position position;
    if (!document_position(params, path, position) || session.snapshot() == nullptr) {
        return std::nullopt;
    }

    const AST::File *file = session.file_of(path);
    if (file == nullptr) {
        return std::nullopt;
    }

    LiveViews views(session);
    std::optional<AST::Location> location = snapshot_location(views, *file, position, utf8);
    return LocatedQuery{ file, std::move(location), std::move(views) };
}

void Compiler::Lsp::Server::Impl::handle_hover(const json &id, const json &params)
{
    auto query = locate_query(params);
    const auto hit = query.has_value() && query->location.has_value()
        ? hover(*session.snapshot(), *query->file, query->location.value())
        : std::nullopt;
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
        json range = live_span_json(query->views, hit->range, utf8);
        if (!range.is_null()) {
            result["range"] = std::move(range);
        }
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_definition(const json &id, const json &params)
{
    auto query = locate_query(params);
    const auto hit = query.has_value() && query->location.has_value()
        ? definition(*session.snapshot(), *query->file, query->location.value())
        : std::nullopt;
    json result = hit.has_value() ? live_location_json(query->views, hit.value(), utf8) : json();
    if (result.is_null()) {
        reply_null(id);
        return;
    }

    reply(id, std::move(result));
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
    auto query = locate_query(params);
    json result = json::array();
    if (!query.has_value() || !query->location.has_value()) {
        reply(id, std::move(result));
        return;
    }

    bool include_declaration = true;
    if (params.contains("context") && params["context"].is_object()) {
        include_declaration = params["context"].value("includeDeclaration", true);
    }

    for (const DefinitionAnswer &hit : references(
            *session.snapshot(), *query->file, query->location.value(), include_declaration)) {
        json loc = live_location_json(query->views, hit, utf8);
        if (!loc.is_null()) {
            result.push_back(std::move(loc));
        }
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
        result.push_back(workspace_symbol_json(symbol, utf8));
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_signature_help(const json &id, const json &params)
{
    auto query = locate_query(params);
    const auto help = query.has_value() && query->location.has_value()
        ? signature_help(*session.snapshot(), *query->file, query->location.value())
        : std::nullopt;
    if (!help.has_value()) {
        reply_null(id);
        return;
    }

    reply(id, signature_help_json(help.value()));
}

void Compiler::Lsp::Server::Impl::handle_document_highlight(const json &id, const json &params)
{
    auto query = locate_query(params);
    json result = json::array();
    if (!query.has_value() || !query->location.has_value()) {
        reply(id, std::move(result));
        return;
    }

    for (const Highlight &highlight : document_highlights(
            *session.snapshot(), *query->file, query->location.value())) {
        json range = live_span_json(query->views, highlight.range, utf8);
        if (!range.is_null()) {
            result.push_back(json{
                { "range", std::move(range) },
                { "kind", static_cast<int>(highlight.kind) } });
        }
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_inlay_hint(const json &id, const json &params)
{
    std::filesystem::path path;
    const AST::File *file = nullptr;
    if (!document_path(params, path) || session.snapshot() == nullptr
        || (file = session.file_of(path)) == nullptr) {
        reply(id, json::array());
        return;
    }

    // the requested range is live lines. the snapshot's lines sit elsewhere, so ask for all of
    // them and keep the hints that land inside it once mapped back
    uint32_t first = 0;
    uint32_t last = std::numeric_limits<uint32_t>::max();
    if (params.contains("range") && params["range"].is_object()) {
        const json &range = params["range"];
        if (range.contains("start") && range["start"].is_object()) {
            first = range["start"].value("line", 0u);
        }
        if (range.contains("end") && range["end"].is_object()) {
            last = range["end"].value("line", last);
        }
    }

    LiveViews views(session);
    const LiveViews::View &view = views.view_of(*file);

    json result = json::array();
    for (const InlayHint &hint : inlay_hints(*session.snapshot(), *file, 1, std::numeric_limits<uint32_t>::max())) {
        const std::optional<AST::Location> live = view.map.location_to_live(hint.position);
        if (!live.has_value()) {
            continue;
        }

        const Position at = view.live.has_value()
            ? live_position_of(view.live.value(), live.value(), utf8)
            : lsp_position_of(*file, live.value(), utf8);
        if (at.line < first || at.line > last) {
            continue;
        }

        result.push_back(inlay_hint_json(at, hint));
    }

    reply(id, std::move(result));
}

void Compiler::Lsp::Server::Impl::handle_completion(const json &id, const json &params)
{
    std::filesystem::path path;
    Position position;
    if (!document_position(params, path, position)) {
        reply(id, json{ { "isIncomplete", false }, { "items", json::array() } });
        return;
    }

    // the editor's text: the line being typed is in no snapshot
    const LiveText live(session.text_of(path));
    const AST::File *file = session.file_of(path);

    CompletionRequest request;
    request.live = &live;
    request.cursor = live.offset_of(live_location_of(live, position, utf8));
    request.snippets = snippet_support;
    request.is_manifest = path.filename() == "module.eco";
    if (file != nullptr && session.snapshot() != nullptr) {
        request.snapshot = session.snapshot();
        request.file = file;
        request.map = LineMap::build(*file, live);
    }

    if (params.contains("context") && params["context"].is_object()) {
        request.from_trigger_character = params["context"].value("triggerKind", 1) == 2;
    }

    const CompletionAnswer answer = completion(request);
    const Position start = live_position_of(live, live.location_of(answer.replace_start), utf8);
    const Position cursor = live_position_of(live, live.location_of(request.cursor), utf8);
    const Position end = live_position_of(live, live.location_of(answer.replace_end), utf8);

    json items = json::array();
    const Range insert{ start, cursor };
    const Range replace{ start, end };
    for (const CompletionItem &item : answer.items) {
        items.push_back(completion_item_json(
            item,
            insert,
            replace,
            label_details_support,
            insert_replace_support
        ));
    }

    reply(id, json{ { "isIncomplete", false }, { "items", std::move(items) } });
}

// the file and position a rename is asked at, or nothing after a refusal has already gone out. a
// rename answers only from a snapshot of exactly the editor's text (the dispatcher compiled first),
// so a compile that failed, or a text it has not caught up with, is a refusal
const AST::File *Compiler::Lsp::Server::Impl::rename_file(
    const json &id,
    const json &params,
    std::filesystem::path &path,
    AST::Location &location
)
{
    Position position;
    const AST::File *file = nullptr;
    if (!document_position(params, path, position) || session.snapshot() == nullptr
        || (file = session.file_of(path)) == nullptr) {
        reply_error(id, -32803, "This file is not part of the compiled project, so there is nothing to rename.");
        return nullptr;
    }

    const Document *open = session.document(path);
    if (session.parse_failure().has_value() || (open != nullptr && (!file->content.has_value() || file->content.value() != open->content))) {
        reply_error(id, -32803, "The file does not compile as it is. Fix the error first, then rename.");
        return nullptr;
    }

    location = echo_location_of(*file, position, utf8);
    return file;
}

void Compiler::Lsp::Server::Impl::handle_prepare_rename(const json &id, const json &params)
{
    std::filesystem::path path;
    AST::Location location;
    const AST::File *file = rename_file(id, params, path, location);
    if (file == nullptr) {
        return;
    }

    const PrepareRenameAnswer answer = prepare_rename(*session.snapshot(), *file, location, session.workspace_root());
    if (!answer.refusal.empty()) {
        reply_error(id, -32803, answer.refusal);
        return;
    }

    reply(id, json{ { "range", span_json(answer.range, utf8) }, { "placeholder", answer.placeholder } });
}

void Compiler::Lsp::Server::Impl::handle_rename(const json &id, const json &params)
{
    std::filesystem::path path;
    AST::Location location;
    const AST::File *file = rename_file(id, params, path, location);
    if (file == nullptr) {
        return;
    }

    const std::string new_name = params.is_object() ? params.value("newName", "") : "";
    const RenameAnswer answer = rename(*session.snapshot(), *file, location, new_name, session.workspace_root());
    if (!answer.refusal.empty()) {
        reply_error(id, -32803, answer.refusal);
        return;
    }

    // every file an edit lands in has to be the text that was compiled, or the ranges are wrong
    std::map<std::string, json> by_uri;
    std::map<std::string, std::optional<int>> versions;
    for (const RenameEdit &edit : answer.edits) {
        const Document *open = session.document(edit.path);
        if (open != nullptr && edit.range.file != nullptr
            && (!edit.range.file->content.has_value() || edit.range.file->content.value() != open->content)) {
            reply_error(id, -32803, edit.path.filename().string() + " has changed since it was compiled. Try again in a moment.");
            return;
        }

        const std::string uri = uri_from_path(edit.path);
        if (!by_uri.contains(uri)) {
            by_uri[uri] = json::array();
            versions[uri] = session.overlay_version(edit.path);
        }

        by_uri[uri].push_back(json{ { "range", span_json(edit.range, utf8) }, { "newText", edit.new_text } });
    }

    reply(id, workspace_edit_json(by_uri, versions, document_changes_support));
}
