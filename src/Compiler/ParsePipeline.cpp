#include "Compiler/ParsePipeline.h"

#include "eco.h"
#include "AST/ASTAccessPass.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ASTFileRoot.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTMonomorphizer.h"
#include "AST/ASTPointerAdjuster.h"
#include "AST/ASTSourceToken.h"
#include "AST/ASTTypeChecker.h"
#include "Compiler/ProgressReporter.h"
#include "Compiler/SettledPath.h"

#if ECO_USE_EMBEDDED_STDLIB
#include "stdlib_embedded.h"
#endif

#include <fmt/core.h>

#include <algorithm>

namespace
{
    Parser::ModuleParser::InputFile input_file_for(
        const std::filesystem::path &path,
        const Compiler::SourceOverride &overlay
    )
    {
        if (overlay) {
            if (const std::optional<std::string> content = overlay(path)) {
                return Parser::ModuleParser::InputFile(path, content.value());
            }
        }

        return Parser::ModuleParser::InputFile(path);
    }

    std::optional<Compiler::FrontEndFailure> parse_one_input(
        Parser::ModuleParser &parser,
        Parser::ModuleParser::InputPayload &input
    )
    {
        // **caught whatever ECO_DONT_CATCH_EXCEPTIONS says**, unlike the tokenization error inside. That
        // macro exists to let a *compiler bug* crash with a stack trace, and a malformed `#[if: ...]` is
        // not one - it is a mistake in the source being compiled, and reporting it as a crash would
        // blame echoc for it.
        //
        // the banner names conditional compilation and covers a malformed `test` header too, that being
        // the other thing Parser::filter_conditional_tokens decides: a test block is a region compiled
        // under one condition, and the filter has to read its header to know where the region it is
        // dropping ends
        try {
            parser.parse_input(input);
        }
        catch (AST::Module::TokenFilterException &e) {
            return Compiler::FrontEndFailure{
                "Conditional Compilation Failed", e.what(), std::nullopt };
        }
        catch (Parser::ModuleParser::TokenizationException &e) {
            std::optional<std::filesystem::path> path;
            if (e.file != nullptr) {
                path = e.file->get_path();
            }

            return Compiler::FrontEndFailure{ "Tokenization Failed", e.what(), std::move(path) };
        }

        return std::nullopt;
    }

#if ECO_USE_EMBEDDED_STDLIB
    void parse_embedded_stdlib_module(AST::Bundle &bundle, Parser::ModuleParser &parser)
    {
        AST::module_handle_t stdlib_handle = bundle.modules.add_module("stdlib");
        auto &stdlib = bundle.modules.get_module(stdlib_handle);

        EmbeddedModule::load_stdlib_module(bundle, stdlib);
        parser.parse_module(stdlib, bundle.collector);
    }
#endif

    void collect_shared_top_level_code(const Parser::ModuleManifest &manifest, AST::Bundle &bundle)
    {
        if (!Compiler::module_declares_a_program(manifest)) {
            return;
        }

        AST::Module *module = bundle.modules.find_module_ptr(manifest.name);

        if (module == nullptr) {
            return;
        }

        for (AST::File &file : module->files()) {
            const bool is_an_entry = std::any_of(manifest.targets.begin(), manifest.targets.end(),
                [&file](const Parser::ModuleTarget &target) {
                    return target.entry == file.get_path();
                });

            const bool is_scoped = !std::binary_search(
                manifest.sources.begin(), manifest.sources.end(), file.get_path());

            if (is_an_entry || is_scoped || file.root == nullptr) {
                continue;
            }

            AST::Node *statement = AST::first_top_level_statement(*file.root);

            if (statement == nullptr) {
                continue;
            }

            const TokenReference *token = AST::source_token_of(*statement);

            if (token == nullptr) {
                continue;
            }

            bundle.collector.collect_issue<AST::Issue::TopLevelCodeOutsideEntry>(
                AST::CodeRef{ module, token->make_slice() },
                manifest.name,
                file.get_path().filename().string());
        }
    }

    std::optional<Compiler::FrontEndFailure> parse_manifest_modules(
        const std::vector<const Parser::ModuleManifest *> &manifests,
        const std::vector<std::filesystem::path> &roots,
        const Parser::ActiveTargets &active_targets,
        const Compiler::SourceOverride &overlay,
        AST::Bundle &bundle,
        Parser::ModuleParser &parser
    )
    {
        for (const Parser::ModuleManifest *entry : manifests) {
            const Parser::ModuleManifest &manifest = *entry;

            Compiler::ProgressStep step(
                Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_parse, manifest.name);

            AST::module_handle_t handle = bundle.modules.add_module(manifest.name);
            auto &module = bundle.modules.get_module(handle);

            auto input = Parser::ModuleParser::InputPayload {
                .files = {},
                .module = module,
                .collector = bundle.collector
            };

            const Parser::ModuleContribution contribution =
                Parser::module_contribution_for(manifest, active_targets);

            for (const auto &source : contribution.sources) {
                input.files.push_back(input_file_for(source, overlay));
            }

            step.summary(fmt::format(
                "{} file{}",
                contribution.sources.size(), contribution.sources.size() == 1 ? "" : "s"));

            if (Compiler::manifest_is_a_root(manifest, roots)) {
                step.detail(contribution.sources);
            }

            if (auto failure = parse_one_input(parser, input)) {
                return failure;
            }

            step.finish(true);
        }

        return std::nullopt;
    }
};

void Compiler::run_semantic_pipeline(
    AST::Bundle &bundle,
    const CompilerOptions &options,
    std::function<void(AST::Monomorphizer &)> after_monomorphize
)
{
    AST::ConstantExpander(bundle).run();

    AST::Monomorphizer monomorphizer(bundle);
    monomorphizer.run();

    if (after_monomorphize) {
        after_monomorphize(monomorphizer);
    }

    AST::PointerAdjuster(bundle).run();
    AST::AccessPass(bundle).run();
    AST::TypeChecker(bundle, options).run();
}

std::optional<std::filesystem::path> Compiler::discover_project_manifest(
    const std::filesystem::path &directory
)
{
    if (directory.empty()) {
        return std::nullopt;
    }

    return Parser::manifest_at(directory);
}

bool Compiler::resolve_front_end_manifests(
    const std::vector<std::string> &named_roots,
    bool with_stdlib,
    bool allow_project_discovery,
    const std::filesystem::path &discovery_dir,
    const std::filesystem::path &package_dir_override,
    Parser::ManifestScratch &scratch,
    std::vector<Parser::ModuleManifest> &out,
    std::vector<std::filesystem::path> &out_roots
)
{
    std::vector<std::filesystem::path> roots;

#if !ECO_USE_EMBEDDED_STDLIB
    if (with_stdlib) {
        roots.push_back(std::filesystem::path(STDLIB_SOURCE_DIR) / "module.eco");
    }
#else
    (void)with_stdlib;
#endif

    const size_t implicit_roots = roots.size();

    for (const std::string &named : named_roots) {
        roots.push_back(std::filesystem::path(named));
    }

    if (roots.size() == implicit_roots && allow_project_discovery) {
        if (const std::optional<std::filesystem::path> discovered =
                discover_project_manifest(discovery_dir)) {
            roots.push_back(discovered.value());
        }
    }

    out_roots.clear();

    for (auto named = roots.begin() + implicit_roots; named != roots.end(); ++named) {
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(*named);

        out_roots.push_back(
            resolved.has_value() ? Compiler::canonical_or_absolute(resolved.value()) : *named);
    }

    if (roots.empty()) {
        return true;
    }

    if (!out_roots.empty()) {
        scratch.package_dir = Parser::resolve_package_dir(
            out_roots.front().parent_path(), package_dir_override);
    }
    else if (!package_dir_override.empty()) {
        scratch.package_dir = Parser::resolve_package_dir({}, package_dir_override);
    }

    return Parser::resolve_module_graph(roots, scratch, out);
}

std::optional<Compiler::FrontEndFailure> Compiler::parse_front_end_bundle(
    const ParseRequest &request,
    AST::Bundle &bundle,
    Parser::ModuleParser &parser
)
{
#if ECO_USE_EMBEDDED_STDLIB
    if (request.with_stdlib) {
        parse_embedded_stdlib_module(bundle, parser);
    }
#else
    (void)request.with_stdlib;
#endif

    if (auto failure = parse_manifest_modules(
            request.manifests,
            request.roots,
            request.active_targets,
            request.overlay,
            bundle,
            parser)) {
        return failure;
    }

    for (const Parser::ModuleManifest *manifest : request.manifests) {
        collect_shared_top_level_code(*manifest, bundle);
    }

    if (request.loose_sources.empty()) {
        return std::nullopt;
    }

    Compiler::ProgressStep step(
        Compiler::ProgressReporter::instance(),
        Compiler::ProgressPhase::t_parse,
        ECO_MAIN_MODULE_NAME);

    AST::module_handle_t module_handle = bundle.modules.add_module(ECO_MAIN_MODULE_NAME);
    auto &module = bundle.modules.get_module(module_handle);

    auto input = Parser::ModuleParser::InputPayload {
        .files = {},
        .module = module,
        .collector = bundle.collector
    };

    for (const auto &source_file : request.loose_sources) {
        input.files.push_back(input_file_for(source_file, request.overlay));
    }

    step.summary(fmt::format(
        "{} file{}", request.loose_sources.size(), request.loose_sources.size() == 1 ? "" : "s"));
    step.detail(request.loose_sources);

    if (auto failure = parse_one_input(parser, input)) {
        return failure;
    }

    step.finish(true);
    return std::nullopt;
}

bool Compiler::module_declares_a_program(const Parser::ModuleManifest &manifest)
{
    return std::any_of(
        manifest.targets.begin(), manifest.targets.end(),
        [](const Parser::ModuleTarget &target) {
            return target.kind == Parser::TargetKind::t_executable;
        });
}

bool Compiler::manifest_claims_main_module(
    const std::vector<const Parser::ModuleManifest *> &manifests
)
{
    return std::any_of(
        manifests.begin(), manifests.end(),
        [](const Parser::ModuleManifest *manifest) {
            return manifest->name == ECO_MAIN_MODULE_NAME;
        });
}

bool Compiler::manifest_claims_main_module(const std::vector<Parser::ModuleManifest> &manifests)
{
    return std::any_of(
        manifests.begin(), manifests.end(),
        [](const Parser::ModuleManifest &manifest) {
            return manifest.name == ECO_MAIN_MODULE_NAME;
        });
}

bool Compiler::manifest_is_a_root(
    const Parser::ModuleManifest &manifest,
    const std::vector<std::filesystem::path> &roots
)
{
    return std::find(roots.begin(), roots.end(), manifest.path) != roots.end();
}

Parser::ActiveTargets Compiler::workspace_analysis_targets(
    const std::vector<Parser::ModuleManifest> &manifests
)
{
    Parser::ActiveTargets active;
    for (const Parser::ModuleManifest &manifest : manifests) {
        Parser::ActiveTargets one = Parser::all_targets_active(manifest);
        for (auto &[module, names] : one) {
            active[module].insert(names.begin(), names.end());
        }
    }

    return active;
}
