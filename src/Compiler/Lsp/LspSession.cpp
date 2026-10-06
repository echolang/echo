#include "Compiler/Lsp/LspSession.h"

#include "Compiler/SettledPath.h"

#include <chrono>
#include <exception>
#include <iterator>
#include <unordered_set>

namespace
{
    std::filesystem::path key_of(const std::filesystem::path &path)
    {
        return Compiler::canonical_or_absolute(path);
    }

    bool is_echo_source(const std::filesystem::path &path)
    {
        const std::string ext = path.extension().string();
        return ext == ".eco" || ext == ".echo";
    }

    bool is_manifest_file(const std::filesystem::path &path)
    {
        return path.filename() == "module.eco";
    }

    int elapsed_ms(std::chrono::steady_clock::time_point start)
    {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
    }
};

Compiler::Lsp::Session::Session(const DriverOptions &driver) :
    _driver(driver)
{
}

Compiler::Lsp::Session::~Session()
{
    wait_worker();
}

void Compiler::Lsp::Session::mark_dirty()
{
    _dirty = true;
    _generation++;
}

void Compiler::Lsp::Session::set_workspace_root(const std::filesystem::path &root)
{
    _workspace_root = key_of(root);
    _resolve_needed = true;
    mark_dirty();
}

void Compiler::Lsp::Session::set_document(
    const std::filesystem::path &path,
    int version,
    std::string content
)
{
    const std::filesystem::path key = key_of(path);
    auto found = _overlay.find(key);
    if (found != _overlay.end() && found->second.content == content) {
        found->second.version = version;
        return;
    }

    _overlay[key] = Document{ std::move(content), version };
    if (is_manifest_file(key)) {
        _resolve_needed = true;
    }

    mark_dirty();
}

void Compiler::Lsp::Session::did_open(
    const std::filesystem::path &path,
    int version,
    std::string content
)
{
    set_document(path, version, std::move(content));
}

void Compiler::Lsp::Session::did_change(
    const std::filesystem::path &path,
    int version,
    std::string content
)
{
    set_document(path, version, std::move(content));
}

void Compiler::Lsp::Session::did_change_watched(const std::vector<WatchedFile> &files)
{
    bool changed = false;

    for (const WatchedFile &file : files) {
        const std::filesystem::path key = key_of(file.path);

        // the manifest is read from disk by resolution even while it is open, so a save is the
        // moment an edit to it takes effect
        if (is_manifest_file(key) || key.filename() == "epm.lock.json") {
            _resolve_needed = true;
            changed = true;
            continue;
        }

        if (!is_echo_source(key)) {
            continue;
        }

        // `#[sources:]` was expanded when the project was resolved. a file made after that is in no
        // module until it is expanded again, and one removed is still listed
        if (file.change != WatchedChange::t_changed) {
            _resolve_needed = true;
            changed = true;
            continue;
        }

        // an open document is the editor's. its bytes arrive through didChange
        if (_overlay.find(key) == _overlay.end()) {
            changed = true;
        }
    }

    if (changed) {
        mark_dirty();
    }
}

void Compiler::Lsp::Session::did_close(const std::filesystem::path &path)
{
    const std::filesystem::path key = key_of(path);
    _overlay.erase(key);
    if (is_manifest_file(key)) {
        _resolve_needed = true;
    }

    mark_dirty();
}

std::optional<std::string> Compiler::Lsp::Session::overlay_content_in(
    const std::map<std::filesystem::path, Document> &overlay,
    const std::filesystem::path &path
) const
{
    const std::filesystem::path key = key_of(path);
    auto found = overlay.find(key);
    if (found != overlay.end()) {
        return found->second.content;
    }

    return std::nullopt;
}

std::optional<int> Compiler::Lsp::Session::overlay_version(const std::filesystem::path &path) const
{
    const std::filesystem::path key = key_of(path);
    auto found = _overlay.find(key);
    if (found != _overlay.end()) {
        return found->second.version;
    }

    return std::nullopt;
}

void Compiler::Lsp::Session::resolve_project()
{
    _project_resolution++;
    _project_issues.clear();

    std::string error;

    if (!Compiler::TargetFacts::resolve(
            _driver.target_os,
            _driver.target_arch,
            _driver.defines,
            _facts,
            error)) {
        _facts_ok = false;
        _manifests.clear();
        _roots.clear();
        _loose_mode = true;
        _project_issues.push_back(ProjectIssue{
            std::nullopt, 0, AST::IssueSeverity::Error, "Invalid Target: " + error });
        return;
    }

    _facts_ok = true;

    Parser::ManifestScratch scratch(_facts);

    const bool resolved = Compiler::resolve_front_end_manifests(
        _driver.modules,
        !_driver.no_stdlib,
        _driver.modules.empty(),
        _workspace_root,
        _driver.package_dir,
        scratch,
        _manifests,
        _roots);

    // every issue from this resolution, including those with a file. a missing dependency drops the
    // project into loose mode and every cross-file reference goes red; the editor needs the line in
    // module.eco that caused that
    for (const auto &issue : scratch.bundle.collector.issues) {
        const AST::Diagnostic diagnostic = AST::to_diagnostic(*issue);

        ProjectIssue out;
        out.severity = diagnostic.severity;
        out.message = diagnostic.message;
        for (const auto &note : diagnostic.notes) {
            out.message += "\n" + note.message;
        }

        if (diagnostic.primary.file != nullptr) {
            out.path = key_of(diagnostic.primary.file->get_path());
            out.line = diagnostic.primary.start.line;
        }

        _project_issues.push_back(std::move(out));
    }

    if (!resolved && _project_issues.empty()) {
        _project_issues.push_back(ProjectIssue{
            std::nullopt, 0, AST::IssueSeverity::Error,
            "The project's module.eco could not be resolved, so every file is checked on its own." });
    }

    _loose_mode = _roots.empty();
}

Parser::ActiveTargets Compiler::Lsp::Session::workspace_targets() const
{
    return Compiler::workspace_analysis_targets(_manifests);
}

std::vector<std::filesystem::path> Compiler::Lsp::Session::loose_sources_from(
    const CompileInputs &inputs
) const
{
    if (inputs.loose_mode) {
        std::vector<std::filesystem::path> sources;
        for (const auto &[path, document] : inputs.overlay) {
            if (is_echo_source(path) && !is_manifest_file(path)) {
                sources.push_back(path);
            }
        }

        return sources;
    }

    if (Compiler::manifest_claims_main_module(inputs.manifests)) {
        return {};
    }

    std::unordered_set<std::string> claimed;
    for (const Parser::ModuleManifest &manifest : inputs.manifests) {
        const Parser::ModuleContribution contribution =
            Parser::module_contribution_for(manifest, inputs.active_targets);

        for (const auto &source : contribution.sources) {
            claimed.insert(key_of(source).string());
        }
    }

    std::vector<std::filesystem::path> sources;
    for (const auto &[path, document] : inputs.overlay) {
        if (!is_echo_source(path) || is_manifest_file(path)) {
            continue;
        }

        if (claimed.count(key_of(path).string()) == 0) {
            sources.push_back(path);
        }
    }

    return sources;
}

std::vector<const Parser::ModuleManifest *> Compiler::Lsp::Session::compiled_manifests(
    const std::vector<Parser::ModuleManifest> &manifests
) const
{
    std::vector<const Parser::ModuleManifest *> out;
    out.reserve(manifests.size());
    for (const Parser::ModuleManifest &manifest : manifests) {
        out.push_back(&manifest);
    }

    return out;
}

Compiler::Lsp::Session::CompileInputs Compiler::Lsp::Session::capture_inputs(
    const std::string &reason
) const
{
    CompileInputs inputs;
    inputs.generation = _generation;
    inputs.reason = reason;
    inputs.overlay = _overlay;
    inputs.facts = _facts;
    inputs.manifests = _manifests;
    inputs.roots = _roots;
    inputs.loose_mode = _loose_mode;
    inputs.active_targets = workspace_targets();
    inputs.with_stdlib = !_driver.no_stdlib;
    inputs.options = _driver.options;
    return inputs;
}

Compiler::Lsp::Session::RebuildProduct Compiler::Lsp::Session::produce_snapshot(
    const CompileInputs &inputs
) const
{
    RebuildProduct product;
    product.generation = inputs.generation;
    product.report.reason = inputs.reason;

    const auto started = std::chrono::steady_clock::now();
    auto next = std::make_unique<Snapshot>();
    next->bundle = std::make_unique<AST::Bundle>();
    Parser::ModuleParser parser(inputs.facts, {});

    const std::vector<const Parser::ModuleManifest *> compiled = compiled_manifests(inputs.manifests);
    const std::vector<std::filesystem::path> loose = loose_sources_from(inputs);

    ParseRequest request{
        compiled,
        inputs.roots,
        inputs.active_targets,
        loose,
        inputs.with_stdlib,
        [&](const std::filesystem::path &path) {
            return overlay_content_in(inputs.overlay, path);
        }
    };

    try {
        const auto parse_started = std::chrono::steady_clock::now();
        const std::optional<FrontEndFailure> failure =
            parse_front_end_bundle(request, *next->bundle, parser);
        product.report.parse_ms = elapsed_ms(parse_started);

        if (failure.has_value()) {
            product.failure = failure;
            product.report.failed = true;
            product.report.total_ms = elapsed_ms(started);
            return product;
        }

        const auto semantic_started = std::chrono::steady_clock::now();
        run_semantic_pipeline(*next->bundle, inputs.options);
        product.report.semantic_ms = elapsed_ms(semantic_started);

        const auto index_started = std::chrono::steady_clock::now();
        next->index.build(*next->bundle);
        product.report.index_ms = elapsed_ms(index_started);

        product.report.modules = static_cast<size_t>(
            std::distance(next->bundle->modules.begin(), next->bundle->modules.end()));
        product.report.files = next->index.files().size();
        product.snapshot = std::move(next);
    }
    catch (const std::exception &e) {
        product.failure = FrontEndFailure{ "", e.what(), std::nullopt };
        product.report.failed = true;
    }

    product.report.total_ms = elapsed_ms(started);
    return product;
}

bool Compiler::Lsp::Session::apply_product(RebuildProduct product)
{
    if (product.generation != _generation) {
        return false;
    }

    if (product.failure.has_value()) {
        _parse_failure = product.failure;
    }
    else {
        _parse_failure.reset();
        if (product.snapshot != nullptr) {
            _snapshot = std::move(product.snapshot);
        }
    }

    return true;
}

void Compiler::Lsp::Session::wait_worker()
{
    if (_worker.joinable()) {
        _worker.join();
    }
}

bool Compiler::Lsp::Session::launch(const std::string &reason)
{
    if (_running.load()) {
        return true;
    }

    if (_resolve_needed) {
        resolve_project();
        _resolve_needed = false;
    }

    if (!_facts_ok) {
        // resolved again on the next edit or file event: the target is settled from the driver
        // options, but a project reported as broken has to be able to stop being broken
        _dirty = false;
        _resolve_needed = true;
        return false;
    }

    wait_worker();

    CompileInputs inputs = capture_inputs(reason);
    _dirty = false;
    _running.store(true);

    _worker = std::thread([this, inputs = std::move(inputs)]() {
        RebuildProduct product = produce_snapshot(inputs);
        {
            std::lock_guard<std::mutex> lock(_finished_mu);
            _finished = std::move(product);
        }
        _running.store(false);
    });

    return true;
}

Compiler::Lsp::RebuildReport Compiler::Lsp::Session::rebuild()
{
    RebuildReport last;
    last.reason = "sync";

    while (true) {
        if (const std::optional<RebuildReport> taken = take_finished_rebuild()) {
            last = taken.value();
        }

        if (_running.load()) {
            wait_worker();
            continue;
        }

        if (_dirty || _snapshot == nullptr) {
            if (!launch("sync")) {
                last.failed = true;
                return last;
            }

            continue;
        }

        return last;
    }
}

void Compiler::Lsp::Session::start_rebuild(const std::string &reason)
{
    if (_running.load()) {
        return;
    }

    launch(reason);
}

bool Compiler::Lsp::Session::rebuild_running() const
{
    return _running.load();
}

std::optional<Compiler::Lsp::RebuildReport> Compiler::Lsp::Session::take_finished_rebuild()
{
    RebuildProduct product;
    {
        std::lock_guard<std::mutex> lock(_finished_mu);
        if (!_finished.has_value()) {
            return std::nullopt;
        }

        product = std::move(_finished.value());
        _finished.reset();
    }

    if (_worker.joinable() && !_running.load()) {
        _worker.join();
    }

    RebuildReport report = product.report;
    apply_product(std::move(product));
    return report;
}

std::vector<std::string> Compiler::Lsp::Session::indexed_paths() const
{
    if (_snapshot == nullptr) {
        return {};
    }

    return _snapshot->index.paths();
}

std::vector<AST::Diagnostic> Compiler::Lsp::Session::diagnostics() const
{
    std::vector<AST::Diagnostic> out;
    if (_snapshot == nullptr) {
        return out;
    }

    for (const auto &issue : _snapshot->bundle->collector.issues) {
        out.push_back(AST::to_diagnostic(*issue));
    }

    return out;
}

const AST::File *Compiler::Lsp::Session::file_of(const std::filesystem::path &path) const
{
    if (_snapshot == nullptr) {
        return nullptr;
    }

    return _snapshot->index.file_for_path(path);
}

const Compiler::Lsp::Document *Compiler::Lsp::Session::document(const std::filesystem::path &path) const
{
    auto found = _overlay.find(key_of(path));
    return found != _overlay.end() ? &found->second : nullptr;
}

std::string Compiler::Lsp::Session::text_of(const std::filesystem::path &path) const
{
    if (const Document *open = document(path)) {
        return open->content;
    }

    const AST::File *file = file_of(path);
    if (file != nullptr && file->content.has_value()) {
        return file->content.value();
    }

    return "";
}
