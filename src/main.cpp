#include <iostream>
#include <fstream>
#include <sstream>

#include <argparse.h>

#include <fmt/core.h>

#include "eco.h"
#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "AST/ASTDiagnosticRenderer.h"
#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ASTMonomorphizer.h"
#include "AST/ASTAccessPass.h"
#include "AST/ASTPointerAdjuster.h"
#include "AST/ASTTypeChecker.h"
#include "Parser/ManifestParser.h"
#include "Parser/ModuleParser.h"
#include "Compiler/BuildLayout.h"
#include "Compiler/CBuild.h"
#include "Compiler/CompilerException.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/ProgressReporter.h"
#include "Compiler/SettledPath.h"
#include "Compiler/TargetSubtarget.h"
#include "Compiler/LLVM/LLVMCompiler.h"

#include <llvm/Support/DynamicLibrary.h>
#include <llvm/TargetParser/Host.h>

#if ECO_USE_EMBEDDED_STDLIB
#include "stdlib_embedded.h"
#endif

#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>
#include <chrono>

// the files an argument names, expanding wildcards. `missing` counts the paths that were named
// literally and are not there - a caller has to refuse those rather than compile what is left, since
// a build that quietly omits one of its inputs is a build that links against nothing
//
// a wildcard matching nothing is deliberately not counted: an empty glob is a pattern with no
// matches, not a file somebody expected to exist
std::vector<std::filesystem::path> get_file_list_from_args(
    argparse::ArgumentParser &cli, const std::string &arg, size_t &missing)
{
    auto path_strings = cli.get<std::vector<std::string>>(arg);

    std::vector<std::filesystem::path> files;

    for (const auto &path_string : path_strings) {
        std::filesystem::path path{path_string};

        // check for wildcards. through the same expander a manifest's `#[sources:]` uses, so a pattern
        // does not mean one thing on the command line and another in a manifest
        if (path_string.find('*') != std::string::npos) {
            auto paths = Parser::expand_source_pattern(path);

            for (const auto &p : paths) {
                if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
                    files.push_back(p);
                }
            }
        } else {
            if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
                files.push_back(path);
            }
            else {
                // named and not there. dropping it silently left the caller reporting "No source
                // files provided" for a path that was very much provided - so one typo in a filename
                // answered with a complaint about something else entirely, and a *second* file on the
                // same command line made the typo vanish without a word
                std::cerr << "Cannot open '" << path_string << "': no such file." << std::endl;
                missing++;
            }
        }
    }

    return files;
}

int handle_parse(
    const AST::DiagnosticRenderer &diagnostics,
    Parser::ModuleParser &parser,
    Parser::ModuleParser::InputPayload &input)
{
    // **caught whatever ECO_DONT_CATCH_EXCEPTIONS says**, unlike the tokenization error inside. That macro
    // exists to let a *compiler bug* crash with a stack trace, and a malformed `#[if: ...]` is not one - it
    // is a mistake in the source being compiled, and reporting it as a crash would blame echoc for it
    try {
        parser.parse_input(input);
    }
    catch (AST::Module::TokenFilterException &e) {
        diagnostics.render_untyped("Conditional Compilation Failed", e.what());
        return 1;
    }
#if !ECO_DONT_CATCH_EXCEPTIONS
    catch (Parser::ModuleParser::TokenizationException &e) {
        diagnostics.render_untyped("Tokenization Failed", e.what());
        return 1;
    }
#endif

    return 0;
}

// the standard library, when it is embedded in the binary rather than read from disk. This is the one
// module that cannot come from a manifest, because there is no file list to read - the sources *are* the
// bytes the generated header carries
//
// leaving the standard library out is leaving one module out, nothing more - nothing downstream
// looks a module up by that name, codegen only ever asks for ECO_MAIN_MODULE_NAME, and the core
// types are bound by whichever source declares `#[core: ...]` rather than by the stdlib. what the
// program gives up is `die`, `assert` and the `mem::`/`std::math::` namespaces, which is the point: a
// test reading the emitted IR or an AST dump does not want several hundred lines of library
// standing between its first assertion and the code it is about
#if ECO_USE_EMBEDDED_STDLIB
static void parse_embedded_stdlib_module(AST::Bundle &bundle, Parser::ModuleParser &parser)
{
    AST::module_handle_t stdlib_handle = bundle.modules.add_module("stdlib");
    auto &stdlib = bundle.modules.get_module(stdlib_handle);

    EmbeddedModule::load_stdlib_module(bundle, stdlib);
    parser.parse_module(stdlib, bundle.collector);
}
#endif

// the manifest of the project the compiler was invoked *in*, when the command line names nothing at all.
//
// this is what makes `echoc run` work the way every other project tool does: a directory holding a
// module.eco is a project, and pointing at it is redundant. Only consulted when neither `-m` nor a source
// file was given, so it can never override something the user asked for
static std::optional<std::filesystem::path> discover_project_manifest()
{
    std::error_code ec;
    const std::filesystem::path here = std::filesystem::current_path(ec);

    if (ec) {
        return std::nullopt;
    }

    // through the one owner of "what manifest does this path name", so the manifest's file name is spelled
    // in exactly one place and a directory is resolved the way every other root is
    return Parser::manifest_at(here);
}

// what a `#[if:]` may see, off the command line. **before the parse**, and that is not a preference: the
// conditional filter runs between lexing and pass 1, so what a condition sees has to be settled before a
// single file is read. Everything here comes off the command line, so there is nothing to wait for.
//
// shared with `echoc clean`, which resolves the same facts for the same reason - a manifest may gate its
// `#[depends:]` behind an `#[if:]`, so the graph reached without them is not the graph the build produced
static bool resolve_target_facts(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    Compiler::TargetFacts &out
)
{
    std::string error;

    if (!Compiler::TargetFacts::resolve(
            cli.present("--target-os") ? cli.get<std::string>("--target-os") : std::string(),
            cli.present("--target-arch") ? cli.get<std::string>("--target-arch") : std::string(),
            cli.get<std::vector<std::string>>("--define"),
            out,
            error)) {
        diagnostics.render_untyped("Invalid Target", error);
        return false;
    }

    return true;
}

// the manifests this invocation builds, in the order the modules have to be parsed: every `-m` the user
// gave, plus the standard library's own, which is an ordinary manifest module like any other
//
// resolving the whole graph here rather than per flag is what makes a dependency implicit: naming a
// library that depends on another pulls the other in, once, in the right order
//
// **the flags arrive as parameters rather than being read off a parser.** `echoc clean` resolves the same
// graph and registers none of them, and a function that reaches into a command line for `source` cannot be
// called by a subcommand that has no such argument
static bool resolve_manifests(
    const std::vector<std::string> &named_roots,
    bool with_stdlib,
    bool allow_project_discovery,
    const AST::DiagnosticRenderer &diagnostics,
    const Compiler::TargetFacts &facts,
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

    // the standard library is not one of *the user's* roots - it is added to every build - so it is
    // deliberately not in out_roots. Whoever asks "what did this invocation point at" must not be told
    // "the standard library"
    const size_t implicit_roots = roots.size();

    for (const std::string &named : named_roots) {
        roots.push_back(std::filesystem::path(named));
    }

    if (roots.size() == implicit_roots && allow_project_discovery) {
        if (const std::optional<std::filesystem::path> discovered = discover_project_manifest()) {
            roots.push_back(discovered.value());
        }
    }

    // **resolved here, once.** A root may name the manifest file or the directory holding one, and
    // Parser::manifest_at is what owns which a directory means - so the answer is taken while both spellings
    // are still in hand rather than re-derived against every manifest later compared against one. What comes
    // back is the path a manifest records as its own, which makes that comparison an equality
    out_roots.clear();

    for (auto named = roots.begin() + implicit_roots; named != roots.end(); ++named) {
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(*named);

        // one entry per root whether or not it resolved: a root naming nothing is a failure
        // resolve_module_graph reports below in its own words, and until then the count is what says
        // whether this invocation pointed at one module or several
        out_roots.push_back(
            resolved.has_value() ? Compiler::canonical_or_absolute(resolved.value()) : *named);
    }

    if (roots.empty()) {
        return true;
    }

    std::string error;
    if (!Parser::resolve_module_graph(roots, facts, out, error)) {
        diagnostics.render_untyped("Module Manifest Error", error);
        return false;
    }

    return true;
}

// is this manifest one of the roots the invocation named, as opposed to one pulled in behind it.
//
// a plain membership test, because resolve_manifests already settled each root to the path a manifest
// records as its own - `-m lib` and `-m lib/module.eco` are one root by the time they get here. Resolving
// per ask instead cost a Parser::manifest_at and an `equivalent` for every (manifest, root) pair, twice
// over, to answer a question about a list that cannot change during an invocation
static bool manifest_is_a_root(
    const Parser::ModuleManifest &manifest,
    const std::vector<std::filesystem::path> &roots
)
{
    return std::find(roots.begin(), roots.end(), manifest.path) != roots.end();
}

// one AST::Module per manifest, parsed completely before the next one starts - which is what the
// topological order above exists for
static int parse_manifest_modules(
    const AST::DiagnosticRenderer &diagnostics,
    const std::vector<Parser::ModuleManifest> &manifests,
    const std::vector<std::filesystem::path> &roots,
    AST::Bundle &bundle,
    Parser::ModuleParser &parser
)
{
    for (const Parser::ModuleManifest &manifest : manifests) {
        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_parse, manifest.name);

        AST::module_handle_t handle = bundle.modules.add_module(manifest.name);
        auto &module = bundle.modules.get_module(handle);

        auto input = Parser::ModuleParser::InputPayload {
            .files = {},
            .module = module,
            .collector = bundle.collector
        };

        for (const auto &source : manifest.sources) {
            input.files.push_back(Parser::ModuleParser::InputFile(source));
        }

        step.summary(fmt::format(
            "{} file{}",
            manifest.sources.size(), manifest.sources.size() == 1 ? "" : "s"));

        // **a module the invocation pointed at lists its files; a dependency reports a count.** the same
        // distinction resolve_manifests already draws, and for the sentence it draws it with: whoever
        // asks what this invocation pointed at must not be told "the standard library". Twenty-one rows
        // of stdlib under every build is that answer given anyway
        if (manifest_is_a_root(manifest, roots)) {
            step.detail(manifest.sources);
        }

        if (handle_parse(diagnostics, parser, input)) {
            return 1;
        }

        step.finish(true);
    }

    return 0;
}

// builds the bundle both `run` and `build` compile: the stdlib module, then the main module with
// the user's sources. one function rather than two copies, because the copies had already drifted
// - `build` never created a stdlib module at all, so any program calling `mem::` or `std::math::`
// compiled under `run` and failed under `build`
static int build_bundle(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    AST::Bundle &bundle,
    Parser::ModuleParser &parser,
    std::vector<Parser::ModuleManifest> &out_manifests,
    std::string &out_entry_module
)
{
#if ECO_USE_EMBEDDED_STDLIB
    if (!cli.get<bool>("--no-stdlib")) {
        parse_embedded_stdlib_module(bundle, parser);
    }
#endif

    // the manifest modules first, in dependency order, and the loose sources after them - so a program on
    // the command line can name anything a manifest declared, and no manifest can name it back. That is
    // the same one-way rule that holds between two manifests, for the same reason
    std::vector<Parser::ModuleManifest> &manifests = out_manifests;
    std::vector<std::filesystem::path> roots;
    {
        Compiler::ScopedPhase phase("resolve manifests");

        // the parser's own facts, not a second resolution: a manifest may gate its `#[sources:]`, so the
        // list of files and the conditions inside those files have to be decided by the same answer
        if (!resolve_manifests(
                cli.get<std::vector<std::string>>("--module"),
                !cli.get<bool>("--no-stdlib"),
                cli.get<std::vector<std::string>>("source").empty(),
                diagnostics, parser.target_facts, manifests, roots)) {
            return 1;
        }
    }

    if (parse_manifest_modules(diagnostics, manifests, roots, bundle, parser) != 0) {
        return 1;
    }

    size_t missing_files = 0;
    auto source_files = get_file_list_from_args(cli, "source", missing_files);

    // a named file that is not there fails the build, even when others compiled. it is an input the
    // user asked for, so carrying on would produce a binary missing whatever was in it - reported by
    // name above, so nothing more is owed here
    if (missing_files > 0) {
        return 1;
    }

    // an entry point has to come from somewhere, and there are two natural spellings because there are two
    // natural shapes of program: a script is a file, an application is a project.
    //
    // loose sources on the command line become the `main` module. Otherwise the program is *the manifest
    // this invocation pointed at* - the one `-m`, or the module.eco in the working directory - whatever it
    // happens to call itself. A build that points at several roots and gives no sources has no answer to
    // "which of these is the program", and guessing would pick one silently
    out_entry_module = ECO_MAIN_MODULE_NAME;

    if (source_files.empty()) {
        if (roots.size() != 1) {
            if (roots.empty()) {
                std::cerr << "No source files provided, and no 'module.eco' in the working directory."
                          << std::endl;
            }
            else {
                std::cerr << "Several manifests were given and no source files, so it is ambiguous which "
                             "module is the program. Name its sources on the command line, or build one "
                             "manifest at a time." << std::endl;
            }
            return 1;
        }

        // the root's own name, resolved through the loaded set rather than from the path - the manifest
        // decides what its module is called. Through manifest_is_a_root and not a comparison of its own,
        // which is what taught it that `-m lib` and `-m lib/module.eco` are one root
        const std::filesystem::path &root = roots.front();
        auto found = std::find_if(manifests.begin(), manifests.end(),
            [&roots](const Parser::ModuleManifest &manifest) {
                return manifest_is_a_root(manifest, roots);
            });

        if (found == manifests.end()) {
            std::cerr << "Internal: the root manifest '" << root.string()
                      << "' is not among the resolved modules." << std::endl;
            return 1;
        }

        out_entry_module = found->name;
    }

    if (!source_files.empty()) {
        const bool manifest_is_the_program = std::any_of(
            manifests.begin(), manifests.end(),
            [](const Parser::ModuleManifest &manifest) { return manifest.name == ECO_MAIN_MODULE_NAME; });

        if (manifest_is_the_program) {
            std::cerr << "A manifest already declares the '" << ECO_MAIN_MODULE_NAME
                      << "' module, so the source files on the command line have nowhere to go."
                      << std::endl;
            return 1;
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

        for (const auto &source_file : source_files) {
            input.files.push_back(Parser::ModuleParser::InputFile(source_file));
        }

        // always listed: these are the files the user named on the command line, so there is no version
        // of this module that is somebody else's dependency
        step.summary(fmt::format(
            "{} file{}", source_files.size(), source_files.size() == 1 ? "" : "s"));
        step.detail(source_files);

        if (handle_parse(diagnostics, parser, input)) {
            return 1;
        }

        step.finish(true);
    }

    // regenerating the embeddable header is a build step, not a compile step. it used to run on
    // every single `echoc run`, which rewrote a tracked file as a side effect of compiling
    if (cli.is_used("--emit-stdlib-header")) {
        if (AST::Module *stdlib = bundle.modules.find_module_ptr("stdlib")) {
            AST::write_embedded_module(*stdlib, STDLIB_SOURCE_DIR "/build/stdlib_embedded.h");
        }
        else {
            std::cerr << "--emit-stdlib-header needs the standard library in the build." << std::endl;
            return 1;
        }
    }

    if (cli.get<bool>("--print-symbol-table")) {
        // two stores, deliberately: the namespace tree holds the types, the function registry
        // holds the overload sets
        std::cout << bundle.collector.namespaces.root().debug_dump_symbols() << std::endl;
        std::cout << "[functions]" << std::endl;
        std::cout << bundle.collector.functions.debug_dump() << std::endl;
    }

    if (cli.get<bool>("--print-ast")) {
        for (const auto &mod : bundle.modules) {
            std::cout << "Module: " << mod->debug_description() << std::endl;
        }
    }

    return 0;
}

// the same dump, after the semantic passes have rewritten the tree. its own flag rather than a
// replacement for -a, because the two answer different questions and both get asked: -a is what the
// parser produced, and this is what codegen will actually walk
//
// that difference is not cosmetic. every implicit thing in this language is made explicit by a pass -
// each auto-deref by AST::PointerAdjuster, each destructor call, retain and release by
// AST::OwnershipPass - and none of them exists yet at -a time. checking that a reference count
// balances means reading the tree *here*
static void print_resolved_ast(argparse::ArgumentParser &cli, AST::Bundle &bundle)
{
    if (!cli.get<bool>("--print-resolved-ast")) {
        return;
    }

    for (const auto &mod : bundle.modules) {
        std::cout << "Module: " << mod->debug_description() << std::endl;
    }
}

// the analysis pipeline between parsing and codegen. shared for the same reason build_bundle is:
// a pass added to one entry point and forgotten in the other is a silent behaviour difference
// tests/helpers.cpp mirrors this list and has to be updated alongside it
static int run_semantic_passes(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    AST::Bundle &bundle,
    const Compiler::CompilerOptions &options
)
{
    Compiler::ProgressStep step(
        Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_semantic_passes);

    // **before the monomorphizer, not inside its fixpoint.** every reference to a compile-time constant
    // becomes a clone of that constant's value here, and AST::OwnershipPass - which runs inside that
    // fixpoint - walks a body exactly once, ever: a reference still in a body when it gets there makes
    // that walk's answer permanent and wrong. Nothing in an expansion depends on what the fixpoint
    // produces, so one pass is all it needs
    AST::ConstantExpander(bundle).run();

    // resolve generics into concrete instances before compilation
    AST::Monomorphizer monomorphizer(bundle);
    monomorphizer.run();

    if (cli.get<bool>("--print-instances")) {
        // **the one dump written while a progress row is live.** Every other one in this file sits
        // between steps, which is the measure of whether the steps are placed right - so this is the
        // whole of the suspend list rather than the first entry in one
        Compiler::ProgressReporter::instance().suspend();
        std::cout << monomorphizer.debug_dump_instances() << std::endl;
    }

    // semantic analysis on the concrete AST: resolves member accesses and call arguments and
    // records located issues, so type errors surface here instead of deep in codegen
    // make the pointer transparency the language promises explicit in the tree: every pointer
    // read in a value position gains a deref node, so from here on result_type() is honest
    AST::PointerAdjuster(bundle).run();

    // addresses may alias, accesses may not conflict: refuse a call that hands one region to two
    // parameters when either of them takes it exclusively. after the adjuster because a path walk
    // wants the tree with every deref in it, and outside the fixpoint because it mints no calls
    AST::AccessPass(bundle).run();

    // the one pass that reads the options: a builtin can be unavailable, and only the command line
    // knows whether this one is
    AST::TypeChecker(bundle, options).run();

    // **closed before anything is printed**, so the row is above the diagnostics that explain it rather
    // than under them. The driver learns the outcome here, from the collector, which is the moment the
    // row can honestly be drawn
    const bool compiled = !bundle.collector.has_critical_issues();
    step.finish(compiled);

    print_resolved_ast(cli, bundle);

    // **stdout is flushed first, always.** Diagnostics go to stderr and a JIT'd program's output goes to
    // stdout; the two are unbuffered and buffered respectively, so a reader that merged them would see
    // them out of order without this
    std::cout.flush();

    bundle.collector.print_issues(diagnostics);

    diagnostics.render_summary(
        bundle.collector.error_count(), bundle.collector.warning_count(), compiled);

    return compiled ? 0 : 1;
}

// resolves every option that describes *the program being compiled* against the subcommand's
// defaults. one function because the two subcommands disagree only about those defaults, and a second
// spelling of the rules would let them drift - `build` is a release build unless told otherwise
//
// the build mode is still orthogonal to -O, and now visibly so: both subcommands read that flag, so
// `build --release` without it emits release *semantics* - no asserts, no null checks - without the
// optimizer having run over them
//
// `--explain-memory` implying `--track-allocations` is settled here rather than at the two places that
// read them, because a report over a counter nothing maintains does not fail - it reads zero forever,
// which is the answer a person hoped for
//
// cannot fail: the mutually exclusive group refuses --debug with --release at parse time
static Compiler::CompilerOptions resolve_options(
    argparse::ArgumentParser &cli,
    Compiler::BuildMode fallback
)
{
    Compiler::CompilerOptions options;

    options.mode = fallback;

    if (cli.get<bool>("--debug")) {
        options.mode = Compiler::BuildMode::t_debug;
    }
    else if (cli.get<bool>("--release")) {
        options.mode = Compiler::BuildMode::t_release;
    }

    options.debug_info = cli.get<bool>("--debug-info");

    // **`-g` alone also means `--no-optimize`**, settled here rather than at the reader for the reason
    // `--explain-memory` implying `--track-allocations` is: the un-implied combination is not what
    // anybody asking for it wanted. Backend::optimize_unit's O2 pipeline reorders, folds and inlines
    // until stepping through the line table walks a program nobody wrote and every local reads
    // <optimized out> - which is a debug build in name only.
    //
    // `-O` is the opposite request said out loud, so it wins: `-g -O` is "debug info over the optimized
    // program", which is a real thing to want and is what DISPFlagOptimized will be there to label
    options.no_optimize = cli.get<bool>("--no-optimize")
        || (options.debug_info && !cli.get<bool>("--optimize"));

    options.no_tbaa = cli.get<bool>("--no-tbaa");

    options.report_allocations = cli.get<bool>("--explain-memory");
    options.track_allocations = options.report_allocations || cli.get<bool>("--track-allocations");

    // the request as written, resolved by Compiler::resolve_subtarget wherever it is needed. Validated
    // in run_front_end beside --target-os, which is where a mistyped flag value is reported
    options.target_cpu = cli.get<std::string>("--target-cpu");
    options.target_features = cli.get<std::string>("--target-features");

    return options;
}


// what a build reuses and what it has to produce.
//
// Only manifest modules are cacheable, and the entry module never is: its unit is where the C `main` is
// created, so serving it from a store would leave codegen with no entry point to attach one to. It is also the
// module being edited, so it would miss every time anyway.
struct ModuleArtifact
{
    // where the freshly compiled object must be written
    std::filesystem::path object;

    // the sidecar to write once that object exists, so the next miss can name what changed
    std::filesystem::path record;
};

struct ModulePlan
{
    // no compilation unit is created for these, and their stored object is linked instead
    std::set<std::string> cached;

    // the objects already on disk, in module order
    std::vector<std::filesystem::path> reused;

    // what each freshly compiled module is to leave behind, by module name. A module absent from this map is
    // not cacheable, and its object goes to a scratch path beside the executable.
    //
    // one map rather than one per artifact: the two paths are decided together and are always both present or
    // both absent, so two containers would only offer a way for them to disagree
    std::map<std::string, ModuleArtifact> emit_to;
};

// decides, per manifest module, whether its object can be reused.
//
// **and prepares the store while it is there**, which is the one thing it writes: a marker is what makes a
// directory removable, and preparing on the miss path alone leaves a fully-cached project's directory
// unmarked - see the call below. So this is where a build provisions, not only where it plans
static ModulePlan plan_module_artifacts(
    const Compiler::BuildLayout &layout,
    const std::vector<Parser::ModuleManifest> &manifests,
    const std::map<std::string, Compiler::ModuleCacheKey> &keys,
    const std::string &entry_module
)
{
    ModulePlan plan;

    for (const Parser::ModuleManifest &manifest : manifests) {
        if (manifest.name == entry_module) {
            continue;
        }

        auto found = keys.find(manifest.name);
        if (found == keys.end()) {
            continue;
        }

        const std::filesystem::path object = Compiler::module_object_path(manifest, found->second, layout);

        // **before the hit, so a store in use always carries its marker**, not only one that was written to
        // today. Preparing on the miss path alone leaves a fully-cached project's directory unmarked and
        // therefore unremovable, which is a `clean` that works only after a build that changed something
        const Compiler::BuildDirState state = layout.prepare_module_dir(manifest);

        // deliberately not gated on that: a read-only store still serves what is already in it, and a build
        // that refused to reuse an object it can see would be slower for no reason at all
        std::error_code ec;
        if (std::filesystem::is_regular_file(object, ec)) {
            plan.cached.insert(manifest.name);
            plan.reused.push_back(object);
            continue;
        }

        // **an unwritable store is not an error.** A read-only library directory, a toolchain installed
        // system-wide, a full disk: the module is compiled to a scratch object and simply not kept. Failing
        // the build over a missed optimization would make a cache a liability
        if (state != Compiler::BuildDirState::t_ready) {
            continue;
        }

        plan.emit_to[manifest.name] = ModuleArtifact{
            object, Compiler::module_inputs_path(manifest, layout) };
    }

    return plan;
}

// **the whole-program path.** An optimized or dumped build folds every unit into one module first, because both
// the O3 pipeline and the IR dump can only look at one - and that is exactly what a per-module object cache
// cannot have. So the two are mutually exclusive by construction rather than by a warning: `-O` and `-p` get
// whole-program optimization and no cache, everything else gets the cache.
static bool wants_whole_program_module(argparse::ArgumentParser &cli)
{
    return cli.get<bool>("--optimize") || cli.get<bool>("--print-ir");
}

// the cache key of every manifest module. Computed whenever anything downstream reads one - the plan, or
// `--explain-cache` - and skipped entirely otherwise, because a key costs a read of every source in the
// build and a whole-program build reuses nothing
static bool compute_cache_keys(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    const std::vector<Parser::ModuleManifest> &manifests,
    const Compiler::CompilerOptions &options,
    const Compiler::TargetFacts &facts,
    std::map<std::string, Compiler::ModuleCacheKey> &out_keys
)
{
    Compiler::ScopedPhase phase("cache keys");

    std::string error;
    if (!Compiler::compute_module_keys(
            manifests, options, facts, cli.get<bool>("--optimize"), out_keys, error)) {
        diagnostics.render_untyped("Module Cache Error", error);
        return false;
    }

    return true;
}

// what the build decided, per module: reused, or compiled and why.
//
// **it reports the plan rather than re-deriving it from the filesystem.** Asking again would be a second
// answer to a question the plan already owns, and the two could disagree - which is exactly the bug this
// diagnostic exists to help find
static void report_cache_plan(
    argparse::ArgumentParser &cli,
    const std::vector<Parser::ModuleManifest> &manifests,
    const std::map<std::string, Compiler::ModuleCacheKey> &keys,
    const ModulePlan &plan,
    const std::string &entry_module_name,
    bool bypassed
)
{
    if (!cli.get<bool>("--explain-cache")) {
        return;
    }

    std::cout << "[cache]" << std::endl;

    if (bypassed) {
        std::cout << "  bypassed: an optimized or dumped build is whole-program, so no module object is "
                     "reusable" << std::endl;
    }

    for (const Parser::ModuleManifest &manifest : manifests) {
        auto found = keys.find(manifest.name);
        if (found == keys.end()) {
            continue;
        }

        const Compiler::ModuleCacheKey &key = found->second;

        std::cout << "  " << manifest.name << "  " << key.hex << "  ";

        if (plan.cached.count(manifest.name) > 0) {
            std::cout << "hit" << std::endl;
            continue;
        }

        std::cout << "miss";

        // three different reasons a module is not being reused, and they have to read differently: it is the
        // program, its store cannot be written, or one of its inputs changed. Only the last is about the source
        auto artifact = plan.emit_to.find(manifest.name);
        if (artifact == plan.emit_to.end()) {
            if (bypassed) {
                std::cout << std::endl;
            }
            else if (manifest.name == entry_module_name) {
                std::cout << "  (the program itself is never cached)" << std::endl;
            }
            else {
                std::cout << "  (its cache directory is not writable)" << std::endl;
            }
            continue;
        }

        const std::string why = Compiler::explain_miss(artifact->second.record, key);
        if (!why.empty()) {
            std::cout << "  (" << why << ")";
        }

        std::cout << std::endl;
    }
}


// emits every module that was not reused, then links the reused objects together with them.
//
// the module's object goes straight into the cache rather than being emitted elsewhere and copied: an object in
// the store is by definition one this compiler just produced for that exact key, and a copy step is one more
// thing that can half-succeed.
static bool emit_and_link_modules(
    LLVMCompiler &compiler,
    const Compiler::BuildLayout &layout,
    const std::string &output,
    const ModulePlan &plan,
    const std::vector<Compiler::LinkRequirement> &link
)
{
    std::vector<std::filesystem::path> objects = plan.reused;

    // a module with nowhere to be stored - the entry module, or anything not from a manifest - gets a
    // scratch object. **inside the build directory and not beside the executable**, which is where these
    // used to land: an `out.main.o` nobody asked for, that no command removed, and that every project's
    // .gitignore had to know about
    const auto object_for = [&](const std::string &module_name) -> std::filesystem::path {
        auto found = plan.emit_to.find(module_name);
        if (found != plan.emit_to.end()) {
            return found->second.object;
        }

        return layout.scratch_object(module_name);
    };

    if (!compiler.emit_objects(object_for, objects)) {
        return false;
    }

    return compiler.link_executable(output, objects, link);
}

// the inputs each freshly emitted module was built from, so the next miss can name what changed.
//
// best effort: a store that cannot be written is a cache that will miss next time, which is slow rather than
// wrong. Refusing the build over it would make an unwritable directory fatal to compiling
static void store_module_records(
    const ModulePlan &plan,
    const std::map<std::string, Compiler::ModuleCacheKey> &keys
)
{
    for (const auto &[module_name, artifact] : plan.emit_to) {
        auto found = keys.find(module_name);
        if (found == keys.end()) {
            continue;
        }

        Compiler::write_inputs_record(artifact.record, found->second);
    }
}

// prints a codegen exception and answers the process status a subcommand returns for it.
//
// one function for the same reason resolve_options is one: both subcommands answer this the same way
// and a second spelling would let them drift. the status is part of the rule, not the caller's - a
// module whose codegen threw part way through a function is neither runnable nor linkable, and
// printing the diagnostic and then carrying on is how both paths used to report success on a failed
// compile: the exit code said 0 while MCJIT ran over a half-built module, or make_exec emitted
// objects for one that may be null or only partly linked. the e2e corpus' `expect:` now asserts it
static int report_compiler_exception(
    const AST::DiagnosticRenderer &diagnostics,
    const Compiler::ASTCompilerException &e
)
{
    std::cout.flush();

    // the same renderer Collector::print_issues uses, so the two cannot drift on how an issue reads
    diagnostics.render_issue(e.issue());
    diagnostics.render_summary(1, 0, /*compiled=*/false);

    return 1;
}

// everything both subcommands do before codegen, and the answers they carry into it.
//
// one function for the same reason build_bundle and run_semantic_passes are one each: a phase added to one
// entry point and forgotten in the other is a silent behaviour difference, and the two tails below are all
// that legitimately differ - `run` merges and JITs, `build` emits objects and links.
struct FrontEnd
{
    std::vector<Parser::ModuleManifest> manifests;
    std::string entry_module;
    Compiler::CompilerOptions options;

    // what the conditional filter was evaluated against. Carried here rather than re-resolved because the
    // module cache folds it into every key, and a key derived from a second resolution is a key that could
    // disagree with the one the parse actually used
    Compiler::TargetFacts target_facts;

    // where every artifact this invocation produces goes. **resolved once**, here, because a second
    // resolution is a second answer and the two can disagree about a directory one of them has already
    // written into
    Compiler::BuildLayout layout;

    // empty unless something downstream reads one - see needs_cache_keys
    std::map<std::string, Compiler::ModuleCacheKey> cache_keys;
};

// where an artifact goes when the program is a loose .eco file rather than a project: there is no manifest
// to put one beside, so nothing here is worth keeping and nothing should be left behind. Per process, so
// two builds running at once do not share a directory one of them is going to remove
static std::filesystem::path fallback_scratch_dir()
{
    return std::filesystem::temp_directory_path() / "echoc" / std::to_string(getpid());
}

// keying a module reads every one of its sources, so it is not free. Only two things ever look at a key: the
// plan, which a whole-program build does not have, and `--explain-cache`
static bool needs_cache_keys(argparse::ArgumentParser &cli, bool whole_program)
{
    return !whole_program || cli.get<bool>("--explain-cache");
}

static bool run_front_end(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    AST::Bundle &bundle,
    Compiler::BuildMode fallback,
    bool whole_program,
    FrontEnd &out
)
{
    if (!resolve_target_facts(cli, diagnostics, out.target_facts)) {
        return false;
    }

    // **the subtarget, checked here and used much later.** it changes nothing the parse can see, so it
    // has no reason to be before it - except that a mistyped `--target-cpu` is a command line mistake,
    // and every other one of those is reported by the driver rather than by whatever stage first trips
    // over it. The answer itself is not carried: Compiler::resolve_subtarget is pure, so the backend
    // and the cache key each ask it and cannot disagree
    {
        Compiler::Subtarget subtarget;
        std::string error;

        if (!Compiler::resolve_subtarget(
                llvm::sys::getDefaultTargetTriple(),
                cli.get<std::string>("--target-cpu"),
                cli.get<std::string>("--target-features"),
                subtarget,
                error)) {
            diagnostics.render_untyped("Invalid Target", error);
            return false;
        }
    }

    // **after the facts and not before**, which is why the parser is built here rather than handed in: it
    // takes them at construction, so there is no window in which one exists that has not been told what
    // platform it is reading for. Neither subcommand touches it once the front end is done
    Parser::ModuleParser parser(out.target_facts);

    {
        Compiler::ScopedPhase phase("parse");
        if (build_bundle(cli, diagnostics, bundle, parser, out.manifests, out.entry_module) != 0) {
            return false;
        }
    }

    // **after build_bundle**, which is where the manifests and the entry module first exist - the layout is
    // a function of both. Deliberately not part of CompilerOptions: everything in there is folded into the
    // module cache key, and a *location* in a key makes the cache go cold for nothing
    {
        auto entry = std::find_if(out.manifests.begin(), out.manifests.end(),
            [&out](const Parser::ModuleManifest &manifest) {
                return manifest.name == out.entry_module;
            });

        out.layout = Compiler::BuildLayout::resolve(
            std::filesystem::path(cli.get<std::string>("--build-dir")),
            entry != out.manifests.end() ? &*entry : nullptr,
            fallback_scratch_dir());

        // **the one refusal a build directory can earn, and it is asked before anything is built.** An
        // unwritable store is never fatal - it is an optimization - but a directory that is somebody else's
        // is a build pointed at the wrong place, and writing into it and reporting nothing is how a later
        // `echoc clean` would delete their work
        const std::string foreign = out.layout.first_foreign_directory(out.manifests);

        if (!foreign.empty()) {
            diagnostics.render_untyped("Build Directory In Use", foreign);
            return false;
        }
    }

    // ahead of the semantic passes, because one of them reads it: AST::TypeChecker refuses
    // `mem::live_allocations()` when nothing is counting. it depends on nothing but the command line, so
    // the only thing its old position bought was the appearance of an order
    out.options = resolve_options(cli, fallback);

    {
        Compiler::ScopedPhase phase("semantic passes");
        if (run_semantic_passes(cli, diagnostics, bundle, out.options) != 0) {
            return false;
        }
    }

    if (needs_cache_keys(cli, whole_program)
        && !compute_cache_keys(
            cli, diagnostics, out.manifests, out.options, out.target_facts, out.cache_keys)) {
        return false;
    }

    return true;
}

// one library `run` has to open, and who to blame if it will not
struct NativeLibrary
{
    std::filesystem::path path;

    // the module that declared the requirement, empty for a `--link` - the sentence below reads
    // differently for each, and a linker's own message can say neither
    std::string declared_by;

    // how it was asked for, as written. `lib:glfw` rather than `libglfw.dylib`, because the manifest line
    // the reader has to go and edit says the former - and for a library no requirement named, the
    // attribute that produced it, for the same reason
    std::string asked_for;
};

// everything this build needs linked, in the order the linker will be given it.
//
// **the manifests in reverse dependency order.** Parser::resolve_module_graph answers dependency-first,
// which is the order the *objects* want; a `-l` is the other way round, because a static archive only
// contributes the members that resolve symbols the linker has already seen. Free to honour and silently
// wrong to skip, since it only shows up against an archive rather than a shared library.
//
// the command line merges **last**, so a manifest's requirement wins the dedup and a `--link search:` has
// the lowest priority of any search path. The valve is for reaching a library nothing declared, and a
// *conflicting* install is better answered by naming the file with `object:`
static bool collect_link_requirements(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    const FrontEnd &front,
    std::vector<Compiler::LinkRequirement> &out
)
{
    for (auto manifest = front.manifests.rbegin(); manifest != front.manifests.rend(); ++manifest) {
        Compiler::merge_link_requirements(manifest->link, out);
    }

    const std::vector<std::string> spelled_on_command_line = cli.get<std::vector<std::string>>("--link");

    if (spelled_on_command_line.empty()) {
        return true;
    }

    // resolved against the working directory, unlike a manifest's: a command line legitimately means
    // "here", where a manifest has to mean the same thing wherever echoc was run from
    const std::filesystem::path here = std::filesystem::current_path();

    std::vector<Compiler::LinkRequirement> from_command_line;

    for (const std::string &spelled : spelled_on_command_line) {
        Compiler::LinkRequirement requirement;
        std::string error;

        if (!Compiler::parse_link_requirement(
                spelled, here, front.target_facts, /*declared_by=*/"", requirement, error)) {
            diagnostics.render_untyped("Invalid Link Requirement", error);
            return false;
        }

        from_command_line.push_back(requirement);
    }

    Compiler::merge_link_requirements(from_command_line, out);

    return true;
}

// what a failed link owes a person beyond whatever the linker already printed.
//
// a linker names a symbol or a library and has no idea which of a build's manifests asked for it, which in
// a program made of a handful of modules leaves the reader with nowhere to start. Each requirement carries
// its declarer for exactly this moment.
//
// **here rather than in the backend**, for the reason the JIT's sibling message is: the sentence needs the
// renderer, and a second ad-hoc printer beside it is a message `--diagnostics=json` cannot consume

// who to blame for a requirement - the reading of `declared_by` both reporters below need. Empty is a
// `--link`, credited to nobody, which is the same emptiness Compiler::link_requirement_spelling switches
// the spelling on
static std::string asker_of(const std::string &declared_by)
{
    return declared_by.empty()
        ? std::string("the command line")
        : fmt::format("module '{}'", declared_by);
}

static void report_link_failure(
    const AST::DiagnosticRenderer &diagnostics,
    const std::vector<Compiler::LinkRequirement> &link
)
{
    std::string blame = "the linker rejected this program.";

    for (const Compiler::LinkRequirement &requirement : link) {
        // the spelling a manifest holds, never the settled value - a note naming `glfw` sends the reader
        // looking for it in a file that says `lib:glfw`
        blame += fmt::format(
            "\n  '{}' was asked for by {}",
            Compiler::link_requirement_spelling(requirement), asker_of(requirement.declared_by));
    }

    diagnostics.render_untyped("Linking Failed", blame);
}

// compiles every module's C sources, and says what the result is *for*.
//
// on a `build` each object joins the link as an `object:` requirement, which is the whole of the wiring: it
// then flows through Compiler::partition_link_requirements into the object group ahead of every library,
// and a failed link names the module that brought it. On a `run` there is no link at all, so the objects go
// into a loadable library instead and the JIT opens it - see Backend::run_code
//
// exactly one of the two is filled, which is what `for_jit` selects - returned together rather than as two
// out-parameters, so neither caller has to name a variable it is going to throw away
struct CModuleBuilds
{
    std::vector<Compiler::LinkRequirement> objects;
    std::vector<NativeLibrary> libraries;
};

static bool build_c_modules(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    const FrontEnd &front,
    bool for_jit,
    CModuleBuilds &out
)
{
    const bool explaining = cli.get<bool>("--explain-cache");

    const std::filesystem::path scratch_dir = front.layout.scratch_cc_dir();

    std::vector<std::string> explain;

    for (const Parser::ModuleManifest &manifest : front.manifests) {
        if (manifest.cc.empty()) {
            continue;
        }

        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_cc, manifest.name);

        const size_t sources = manifest.cc.sources.size();
        step.summary(fmt::format("{} source{}", sources, sources == 1 ? "" : "s"));

        const std::filesystem::path cache_dir = front.layout.module_cc_dir(manifest);

        Compiler::CBuildResult result;
        std::string error;

        if (!Compiler::build_c_sources(
                manifest.cc, front.options, cache_dir, scratch_dir / manifest.name,
                explain, result, error)) {
            diagnostics.render_untyped("C Build Failed", error);
            return false;
        }

        if (!for_jit) {
            for (const std::filesystem::path &object : result.objects) {
                out.objects.push_back(Compiler::LinkRequirement{
                    Compiler::LinkScheme::t_object, object.string(), manifest.name });
            }

            step.finish(true);
            continue;
        }

        // **the module's own link line, not the build's.** a shim calling into GLFW has to resolve those
        // symbols when the library is *loaded*, and the requirements another module declared have nothing
        // to do with it
        std::vector<std::filesystem::path> own_objects;
        std::vector<std::string> link_words;
        Compiler::partition_link_requirements(manifest.link, own_objects, link_words);

        std::filesystem::path library;

        if (!Compiler::build_c_shared_library(
                manifest.cc, result, link_words, cache_dir, scratch_dir / manifest.name,
                library, error)) {
            diagnostics.render_untyped("C Build Failed", error);
            return false;
        }

        // the attribute that produced it, spelled the way the manifest spells it: this library was never
        // asked for by name, so quoting a link spelling would send its reader looking for a line nothing
        // contains
        out.libraries.push_back(NativeLibrary{ library, manifest.name, "#[cc: sources]" });
        step.finish(true);
    }

    if (explaining && !explain.empty()) {
        std::cout << "[cc cache]" << std::endl;
        for (const std::string &line : explain) {
            std::cout << line << std::endl;
        }
    }

    return true;
}

// the human half of a library that would not open.
//
// **the whole of why this is in the driver rather than in Backend::run_code.** The backend can call
// LoadLibraryPermanently perfectly well - the registry it loads into is process-global, so where it happens
// does not matter - but it has neither the requirement's declaring module nor the renderer, and this
// message needs both
static void report_unloadable_library(
    const AST::DiagnosticRenderer &diagnostics,
    const NativeLibrary &library,
    const std::string &reason
)
{
    diagnostics.render_untyped("Cannot Run This Program", fmt::format(
        "{} asked to link '{}', and '{}' could not be loaded: {}\n\n"
        "'echoc run' resolves a program's external symbols inside this process, so a declared library "
        "has to be openable before the program starts. If it is installed somewhere the loader does not "
        "search, name the directory: 'echoc run --link search:<dir>'.",
        asker_of(library.declared_by), library.asked_for, library.path.string(), reason));
}

// resolves every requirement to a file and opens it, before the JIT exists.
//
// **a failure is fatal, and that is the whole of what this function is for.** MCJIT resolves an external
// out of the running process and nothing else ever puts one there - so a declared library that will not
// open is a program whose symbols cannot resolve, and carrying on past a warning does not degrade to a
// useful error: it hangs inside the engine with the note scrolled off the top.
//
// **a requirement with no runtime spelling is reported too.** An `object:` cannot be opened at all, and
// dropping it silently leaves the same hang with nothing said about the declaration that was never applied
static bool load_native_libraries(
    const AST::DiagnosticRenderer &diagnostics,
    const std::vector<Compiler::LinkRequirement> &link,
    const std::vector<NativeLibrary> &already_built
)
{
    std::vector<NativeLibrary> libraries = already_built;
    bool refused = false;

    for (const Compiler::LinkRequirement &requirement : link) {
        std::string refusal;

        if (const auto resolved = Compiler::runtime_library_of(requirement, link, refusal)) {
            libraries.push_back(NativeLibrary{
                resolved.value(),
                requirement.declared_by,
                Compiler::link_requirement_spelling(requirement) });
            continue;
        }

        if (!refusal.empty()) {
            diagnostics.render_untyped("Cannot Run This Program", refusal);
            refused = true;
        }
    }

    for (const NativeLibrary &library : libraries) {
        std::string reason;

        // LoadLibraryPermanently puts the symbols into the process's own search list, which is the only
        // list MCJIT's resolver consults - so this has to happen before the engine finalizes, and being
        // ahead of run_code entirely is the version of that ordering nobody can get wrong
        if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(library.path.string().c_str(), &reason)) {
            report_unloadable_library(diagnostics, library, reason);
            refused = true;
        }
    }

    return !refused;
}

// what a JIT'd program should see as its own `argv[0]`
//
// the source file the invocation named, because that is the closest thing a program with no process of
// its own has to a path it was started from. `echoc` is emphatically the wrong answer: it is the
// process but not the program, and handing it over would have `env::exe()` name the compiler. A
// manifest-only invocation names no source file, and then the module root is the best there is - and
// `fallback` covers the last case, a manifest discovered rather than asked for
std::string program_name(argparse::ArgumentParser &cli, const std::string &fallback)
{
    const std::vector<std::string> sources = cli.get<std::vector<std::string>>("source");
    if (!sources.empty()) {
        return sources.front();
    }

    const std::vector<std::string> modules = cli.get<std::vector<std::string>>("--module");
    if (!modules.empty()) {
        return modules.front();
    }

    return fallback;
}

// the whole-program optimizer, run iff `-O` asked for it - the same answer for both subcommands.
//
// read off the flag rather than off the resolved options. On `build` this used to be unconditional, which
// made `-O` a switch it accepted and silently ignored, and left no way at all to see what codegen emitted
// for a release build, since `-p` only ever showed the optimizer's output
static void optimize_if_asked(argparse::ArgumentParser &cli, LLVMCompiler &compiler)
{
    if (!cli.get<bool>("--optimize")) {
        return;
    }

    Compiler::ScopedPhase phase("optimize");
    Compiler::ProgressStep step(
        Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_optimize);

    compiler.optimize();
    step.finish(true);
}

int main_run(
    argparse::ArgumentParser &cli,
    const AST::DiagnosticRenderer &diagnostics,
    const std::vector<std::string> &program_arguments,
    const char *const *environment)
{
    // **said rather than ignored, and said before anything else.** `-g` is declared on both subcommands so
    // that resolve_options stays one reader of one flag set, but only `build` writes an object a debugger
    // can open - the JIT keeps its module in memory and MCJIT registers nothing a debugger reads. A flag
    // that silently does nothing is the worse failure here: the metadata really is emitted, so nothing
    // downstream is wrong, and the person is left concluding the feature is broken.
    //
    // read off the CLI rather than off the resolved options, for two reasons that agree: this is a
    // question about the *invocation* rather than about the program being compiled, and the options are
    // not resolved until run_front_end has already printed the summary - under json a diagnostic after
    // that one breaks the "diagnostics, then one summary" shape of the stream
    if (cli.get<bool>("--debug-info")) {
        diagnostics.render_untyped(
            "Debug Info Ignored",
            "'-g' produces no artifact a debugger can open on 'run': the JIT emits no object file. "
            "Use 'echoc build -g' and open the resulting executable instead.",
            AST::IssueSeverity::Warning);
    }

    // the one number the checklist's closing line reports, and the only clock in this function.
    // Deliberately not Compiler::PhaseTimings, which measures nothing unless `-t` asked it to
    const auto started = std::chrono::steady_clock::now();

    auto bundle = AST::Bundle();

    // `run` reuses nothing: the JIT is handed one module, so every unit is merged and there are no per-module
    // objects to store or load. Feeding it stored objects instead would mean handing the JIT one per cached
    // module beside main's, which is a question about duplicate weak symbols rather than about caching
    FrontEnd front;
    if (!run_front_end(cli, diagnostics, bundle, Compiler::BuildMode::t_debug, /*whole_program=*/true, front)) {
        return 1;
    }

    const Compiler::CompilerOptions options = front.options;
    const std::string &entry_module = front.entry_module;

    report_cache_plan(cli, front.manifests, front.cache_keys, ModulePlan{}, entry_module, /*bypassed=*/true);

    // **before codegen**, because a C source that does not compile is a build that is going to fail either
    // way, and finding out after the whole Echo program has been lowered wastes the wait. It also puts the
    // `cc` phase where `-t` reads naturally: beside `parse`, not inside `jit`
    std::vector<Compiler::LinkRequirement> link;
    CModuleBuilds c_builds;

    if (!collect_link_requirements(cli, diagnostics, front, link)) {
        return 1;
    }

    {
        Compiler::ScopedPhase phase("cc");

        if (!build_c_modules(
                cli, diagnostics, front, /*for_jit=*/true, c_builds)) {
            return 1;
        }
    }

    if (!load_native_libraries(diagnostics, link, c_builds.libraries)) {
        return 1;
    }

    LLVMCompiler compiler(options);
    compiler.set_entry_module(entry_module);

    try {
        Compiler::ScopedPhase phase("codegen");

        // **inside the try, deliberately.** Unwinding destroys it before the catch below runs, so the
        // failed row is committed ahead of the diagnostic that explains it - which is the order every
        // other step reaches by calling finish() and this one gets for nothing
        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_codegen);

        compiler.compile_bundle(bundle);

        // the JIT can only be handed one module, so `run` always merges
        compiler.link_into_main();
        step.finish(true);
    } catch (Compiler::ASTCompilerException &e) {
        return report_compiler_exception(diagnostics, e);
    }

    optimize_if_asked(cli, compiler);

    if (cli.get<bool>("--print-ir")) {
        compiler.printIR(false);
    }

    // `argv[0]` is the program's own name, and under `run` the honest answer is the source file the
    // entry module was read from - not `echoc`, which is the process but not the program. So the tail
    // the driver split off a `--` is prepended with it rather than used as-is
    std::vector<std::string> argv = { program_name(cli, entry_module) };
    argv.insert(argv.end(), program_arguments.begin(), program_arguments.end());

    // the JIT prunes the module to what the entry point reaches before it runs anything - see
    // Backend::prune_to_entry, which is where that has to live to be sound and is why `-p` above still
    // prints the whole of what codegen emitted
    // **the checklist ends here, and `jit` gets no row.** The compile is over the moment the program
    // starts: a row completing after the program's own output would put the compiler's summary inside
    // the program's conversation, which is the thing "stdout under run belongs to the program" exists to
    // prevent. The cost is that a slow finalizeObject shows nothing, and it is the right trade
    Compiler::ProgressReporter::instance().close(
        fmt::format("compiled '{}'", entry_module), Compiler::progress_elapsed_ms(started));

    int status = 0;
    {
        Compiler::ScopedPhase phase("jit");
        status = compiler.run_code(argv, environment);
    }

    // after the program, because the prune happened inside the run - the same position `[timings]` takes,
    // and for the same reason
    if (cli.get<bool>("--explain-prune")) {
        std::cout << compiler.prune_report();
    }

    std::cout << Compiler::PhaseTimings::instance().report();

    // after the program, because a C module's loadable library lives there and was open for the whole of it
    front.layout.discard_temporary_scratch();

    // the program's own status, so `echoc run` exits the way it did. Today that is always 0 on this
    // path - the entry point's epilogue returns 0 and every other ending goes through libc's `exit`
    // from inside the JIT, which never comes back here at all
    return status;
}

int main_build(argparse::ArgumentParser &cli, const AST::DiagnosticRenderer &diagnostics)
{
    // see main_run: the checklist's own clock, and the only one on this path when `-t` is off
    const auto started = std::chrono::steady_clock::now();

    auto bundle = AST::Bundle();

    // **before anything is compiled.** This used to sit after codegen and after the optimizer, so a
    // forgotten `-o` threw away the whole compile to say one sentence - and the output path is now an
    // input to the decision of what to emit at all, not just where to put it
    if (!cli.present("-o")) {
        std::cerr << "No output file specified." << std::endl;
        return 1;
    }

    const bool whole_program = wants_whole_program_module(cli);

    FrontEnd front;
    if (!run_front_end(cli, diagnostics, bundle, Compiler::BuildMode::t_release, whole_program, front)) {
        return 1;
    }

    const Compiler::CompilerOptions options = front.options;
    const std::string &entry_module = front.entry_module;

    // an optimized or dumped build reuses nothing and stores nothing - see wants_whole_program_module
    const ModulePlan plan = whole_program
        ? ModulePlan{}
        : plan_module_artifacts(front.layout, front.manifests, front.cache_keys, entry_module);

    report_cache_plan(cli, front.manifests, front.cache_keys, plan, entry_module, whole_program);

    // **a reused module gets a row and a rebuilt one does not.** Work that did not happen is the
    // surprising half; which input changed for the ones that did is `--explain-cache`'s question, and
    // answering it twice would put explain_miss's reasoning in two places
    for (const Parser::ModuleManifest &manifest : front.manifests) {
        if (plan.cached.count(manifest.name) > 0) {
            Compiler::ProgressReporter::instance().row(
                Compiler::ProgressPhase::t_cached, manifest.name, "reused",
                Compiler::ProgressState::t_skipped);
        }
    }

    const std::string output = cli.get<std::string>("-o");

    // before codegen, for the reason `run` states: a C source that does not compile fails this build
    // whatever the Echo half does, and the wait is the same either way
    std::vector<Compiler::LinkRequirement> link;

    if (!collect_link_requirements(cli, diagnostics, front, link)) {
        return 1;
    }

    {
        Compiler::ScopedPhase phase("cc");

        CModuleBuilds c_builds;

        if (!build_c_modules(
                cli, diagnostics, front, /*for_jit=*/false, c_builds)) {
            return 1;
        }

        // ahead of every library, which partition_link_requirements is what guarantees
        Compiler::merge_link_requirements(c_builds.objects, link);
    }

    LLVMCompiler compiler(options);
    compiler.set_entry_module(entry_module);

    try {
        Compiler::ScopedPhase phase("codegen");

        // inside the try for the reason main_run's is
        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_codegen);

        compiler.compile_bundle(bundle, plan.cached);

        if (whole_program) {
            compiler.link_into_main();
        }

        step.finish(true);
    } catch (Compiler::ASTCompilerException &e) {
        return report_compiler_exception(diagnostics, e);
    }

    optimize_if_asked(cli, compiler);

    if (cli.get<bool>("--print-ir")) {
        compiler.printIR(false);
    }

    // after the merge decision above and before the emit below, so what it prints is what gets written.
    // it optimizes the units it prints unless told not to - the same call emit_objects makes, so the dump
    // and the object cannot disagree
    if (cli.get<bool>("--print-unit-ir")) {
        compiler.print_unit_ir();
    }

    // **unlike a store, this one is not optional.** A cache that cannot be written costs a rebuild; a
    // scratch directory that cannot be written is an object with nowhere to go, so it is worth a sentence
    // here rather than an ofstream failure from inside the backend
    if (front.layout.prepare_scratch_dir() != Compiler::BuildDirState::t_ready) {
        diagnostics.render_untyped("Cannot Build Here", fmt::format(
            "'{}' could not be created, and the objects on the way to '{}' have nowhere to go.",
            front.layout.scratch_dir().string(), output));
        return 1;
    }

    {
        Compiler::ScopedPhase phase("emit + link");
        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(),
            Compiler::ProgressPhase::t_emit,
            std::filesystem::path(output).filename().string());

        // one merged module, one object, one link for the whole-program path, which has to keep existing
        // because `-O` depends on it. Either way the tool that failed has already said what it could, and
        // what it could not say is which manifest asked for each requirement
        const bool linked = whole_program
            ? compiler.make_exec(output, front.layout.scratch_object(entry_module), link)
            : emit_and_link_modules(compiler, front.layout, output, plan, link);

        // closed before the failure is reported rather than by the destructor after it, so the row sits
        // above the sentence that explains it - the order every other step reaches deliberately
        step.finish(linked);

        if (!linked) {
            report_link_failure(diagnostics, link);
            return 1;
        }
    }

    // only now, and only for what was actually emitted: a record written before the object exists would
    // describe a build that may still have failed
    if (!whole_program) {
        store_module_records(plan, front.cache_keys);
    }

    // **after the link succeeded, and only then.** A failed build is one somebody is about to look at, and
    // the objects it got as far as producing are part of what there is to look at
    front.layout.discard_temporary_scratch();

    Compiler::ProgressReporter::instance().close(
        fmt::format("built '{}'", std::filesystem::path(output).filename().string()),
        Compiler::progress_elapsed_ms(started));

    std::cout << Compiler::PhaseTimings::instance().report();

    return 0;
}

// what an older echoc left beside a manifest, which nothing reads any more.
//
// **named rather than removed.** It carries no marker, so nothing proves it is ours, and recognising it by
// its *shape* instead - "everything in it matches <name>-<hex>.o or <name>.inputs" - would be a second
// spelling of the layout living inside the one function that deletes a directory. A sentence costs nothing
// and tells the person why the directory is on their disk
static void note_legacy_directory(const Parser::ModuleManifest &manifest, std::vector<std::string> &out)
{
    const std::filesystem::path legacy = manifest.directory / ".echo";

    std::error_code ec;
    if (!std::filesystem::is_directory(legacy, ec)) {
        return;
    }

    out.push_back(fmt::format(
        "  note: '{}' is from an older echoc and is no longer read - delete it by hand.",
        legacy.string()));
}

// removes what a build produced, so the next one starts from nothing.
//
// **it parses no source and runs no pass.** Resolving the manifest graph is the whole of what it needs -
// which module directories exist is a function of the manifests and the flags, and reading a single .eco
// file to answer it would make `clean` fail on a program that does not compile
int main_clean(argparse::ArgumentParser &cli, const AST::DiagnosticRenderer &diagnostics)
{
    Compiler::TargetFacts facts;

    if (!resolve_target_facts(cli, diagnostics, facts)) {
        return 1;
    }

    std::vector<Parser::ModuleManifest> manifests;
    std::vector<std::filesystem::path> roots;

    // the standard library is resolved whether or not it is being removed: `--stdlib` needs its directory
    // to name it, and leaving it out would make the line saying it was kept impossible to write
    if (!resolve_manifests(
            cli.get<std::vector<std::string>>("--module"),
            /*with_stdlib=*/true,
            /*allow_project_discovery=*/true,
            diagnostics, facts, manifests, roots)) {
        return 1;
    }

    const Compiler::BuildLayout layout = Compiler::BuildLayout::resolve(
        std::filesystem::path(cli.get<std::string>("--build-dir")), nullptr, {});

    const bool including_stdlib = cli.get<bool>("--stdlib");
    const bool dry_run = cli.get<bool>("--dry-run");

    // **the standard library is always in the graph and is never what was asked for.** So "is there
    // anything to clean" is a question about the rest of it - an empty list here means the invocation
    // pointed at nothing, not that the graph came back empty
    const bool project_present = std::any_of(manifests.begin(), manifests.end(),
        [](const Parser::ModuleManifest &manifest) {
            return !Compiler::is_compiler_supplied_module(manifest);
        });

    if (!project_present && !including_stdlib) {
        std::cerr << "Nothing to clean: no 'module.eco' in the working directory, and no -m was given."
                  << std::endl;
        return 0;
    }

    // padded to the widest of each, the way `--explain-cache` lines up its columns - a list of directories
    // is read down rather than across, and the outcome is what somebody is scanning for
    size_t name_width = 0;
    size_t path_width = 0;

    for (const Parser::ModuleManifest &manifest : manifests) {
        name_width = std::max(name_width, manifest.name.size());
        path_width = std::max(path_width, layout.module_dir(manifest).string().size());
    }

    std::vector<std::string> notes;
    size_t removed = 0;
    bool refused = false;

    std::cout << "[clean]" << std::endl;

    for (const Parser::ModuleManifest &manifest : manifests) {
        const std::filesystem::path directory = layout.module_dir(manifest);

        const std::string shown = directory.string();

        std::cout << "  " << manifest.name << std::string(name_width - manifest.name.size(), ' ')
                  << "  " << shown << std::string(path_width - shown.size(), ' ') << "  ";

        note_legacy_directory(manifest, notes);

        // **the toolchain's store is not this project's to empty.** It is shared by every project on the
        // machine and is the most expensive thing in any build to produce again, so removing it is asked
        // for by name rather than included in a project's tidy-up
        if (Compiler::is_compiler_supplied_module(manifest) && !including_stdlib) {
            std::cout << "kept (pass --stdlib to remove it)" << std::endl;
            continue;
        }

        if (dry_run) {
            std::error_code ec;
            std::cout << (std::filesystem::exists(directory, ec) ? "would remove" : "nothing to remove")
                      << std::endl;
            continue;
        }

        std::string reason;

        switch (layout.remove_module_dir(manifest, reason)) {
        case Compiler::BuildDirRemoval::t_removed:
            std::cout << "removed" << std::endl;
            removed++;
            break;

        case Compiler::BuildDirRemoval::t_absent:
            std::cout << "nothing to remove" << std::endl;
            break;

        // **a refusal is not a skip.** The person asked for a build that starts from nothing and did not
        // get one, so saying so and exiting non-zero is the only honest ending
        case Compiler::BuildDirRemoval::t_refused:
            std::cout << "refused" << std::endl;
            notes.push_back("  " + reason);
            refused = true;
            break;
        }
    }

    for (const std::string &note : notes) {
        std::cout << note << std::endl;
    }

    if (!dry_run) {
        std::cout << fmt::format("removed {} build director{}.", removed, removed == 1 ? "y" : "ies")
                  << std::endl;
    }

    return refused ? 1 : 0;
}

// `envp` is taken rather than reached for, and that is the whole reason `std::env` needs no platform
// conditionals: the environment block arrives as a parameter on every platform we target, whereas the
// `environ` symbol it would otherwise have to read is spelled `_NSGetEnviron()` on Darwin and is not
// portably addressable from IR anywhere. `run` forwards this to the JIT'd program; a `build`'s binary
// gets its own from the OS
int main(int argc, char *argv[], char *envp[])
{
    // **everything after a bare `--` belongs to the program, not to echoc.** Split it off before
    // argparse ever sees it: `source` is declared `.remaining()`, so it would otherwise swallow the
    // whole tail as filenames and `echoc run p.eco -- a b` would look for a file called `a`
    std::vector<std::string> program_arguments;
    int echoc_argc = argc;

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--") {
            echoc_argc = i;
            program_arguments.assign(argv + i + 1, argv + argc);
            break;
        }
    }

    // the version goes to every parser, not just the top-level one: argparse registers `-v`/`--version`
    // on each of them out of its default argument set, and a subparser left to its own devices answers
    // with the library's placeholder "1.0"
    argparse::ArgumentParser cli("echoc", ECO_VERSION_STRING);
    cli.add_description("The echo programming language compiler");

    argparse::ArgumentParser run_command("run", ECO_VERSION_STRING);
    run_command.add_description("Runs the given source files.");
    run_command.add_argument("source")
        .default_value(std::vector<std::string>{})
        .help(".eco source files to be parsed, compiled and run.")
        .remaining();

    argparse::ArgumentParser build_command("build", ECO_VERSION_STRING);
    build_command.add_description("Builds the given source files.");
    build_command.add_argument("source")
        .default_value(std::vector<std::string>{})
        .help(".eco source files to be parsed and compiled.")
        .remaining();

    build_command.add_argument("-o", "--output")
        .help("Output file name.");

    // **deliberately not in the shared flag loop below.** `clean` compiles nothing, so `-O`, `-g`, `-p` and
    // the rest would be flags it accepts and silently ignores - the thing the comment on `--explain-prune`
    // says is worse than rejecting them. It registers what it reads and nothing else
    argparse::ArgumentParser clean_command("clean", ECO_VERSION_STRING);
    clean_command.add_description(
        "Removes what a build produced, so the next one starts from nothing.");

    clean_command.add_argument("--stdlib")
        .help("Also remove the standard library's store. It is shared by every project on this machine "
              "and is the slowest thing to build again, so a project's clean leaves it alone.")
        .default_value(false)
        .implicit_value(true);

    clean_command.add_argument("-n", "--dry-run")
        .help("Print what would be removed, and remove nothing.")
        .default_value(false)
        .implicit_value(true);

    // the third measurement dump, and the only one not shared: `build` never prunes, because the prune is
    // part of the JIT and a per-module object may not depend on what reaches `main`. A flag a subcommand
    // accepts and silently ignores is worse than one it rejects - `-O` was exactly that on `build` - so
    // this is registered where it means something and nowhere else. `-o` above is the same asymmetry the
    // other way round
    run_command.add_argument("-ep", "--explain-prune")
        .help("Print what the JIT prune dropped, and what the entry point still reaches.")
        .default_value(false)
        .implicit_value(true);

    // **what every subcommand needs, `clean` included.** These are the flags that decide which modules an
    // invocation is talking about and where their artifacts are - not what is compiled or how - so a
    // subcommand that compiles nothing still has to answer them the same way, or `echoc clean` would look
    // in a different place than the `echoc build` it is undoing
    for (auto &command : {std::ref(run_command), std::ref(build_command), std::ref(clean_command)}) {
        // repeatable, and the graph is resolved as a whole - so naming a library that depends on another
        // pulls the other in too, once, ahead of it. A dependency does not have to be spelled here
        command.get().add_argument("-m", "--module")
            .help("Build a module from its manifest. May be given more than once; a manifest may be an "
                  "'module.eco' file or a directory holding one.")
            .default_value(std::vector<std::string>{})
            .append()
            .metavar("MANIFEST");

        command.get().add_argument("--build-dir")
            .help("Where build artifacts are written. Defaults to 'ecobuild' beside each manifest, or to "
                  "whatever that manifest's '#[build_dir: ...]' names. One directory for the whole build, "
                  "with a subdirectory per module.")
            .default_value(std::string(""));

        // what `#[if: ...]` regions are evaluated against. Repeatable, bare names only - a define is a
        // flag a condition can test and carries no value, because naming a *value* is what `const`
        // declarations are for
        command.get().add_argument("--define")
            .help("Declare a flag that '#[if: NAME]' can test. May be given more than once.")
            .default_value(std::vector<std::string>{})
            .append();

        // **not cross-compilation.** These change what a condition sees and nothing else: the code is
        // still compiled for the host, so asking for a foreign OS will usually fail at link. They exist so
        // that a test can assert what *another* platform's branch does without owning that platform, which
        // is the only way a `#[if: os == linux]` region is ever checked on a Mac
        command.get().add_argument("--target-os")
            .help("Evaluate conditions as if targeting this OS. Does not cross-compile.");

        command.get().add_argument("--target-arch")
            .help("Evaluate conditions as if targeting this architecture. Does not cross-compile.");

        // **how a diagnostic is drawn, and the one flag an editor sets.** `auto` picks the drawn form
        // when stderr can render it and the plain one otherwise, which is what makes a CI log, a Windows
        // console and a golden test all get the same ASCII without anybody configuring it.
        //
        // `json` is here rather than under a flag of its own because a user choosing it is choosing what
        // comes out of the same stream, and a second flag would let `--json --diagnostics=pretty` be
        // asked. Deliberately **not** in the module cache key - it changes nothing about an emitted
        // object, and a key that reacted to it would go cold for no reason
        command.get().add_argument("--diagnostics")
            .help("How diagnostics are rendered: auto, pretty, ascii or json (one object per line).")
            .default_value(std::string("auto"));

        command.get().add_argument("--color", "--colour")
            .help("Colourise diagnostics: auto, always or never. NO_COLOR and CLICOLOR_FORCE are honoured.")
            .default_value(std::string("auto"));

        // **it silences the checklist and nothing else.** A diagnostic, a warning and every `-t`/`-ec`/
        // `-ep`/`-a`/`-ar`/`-syt`/`-pi` dump are untouched: those were asked for by name, and a flag that
        // could be ignored because another flag was set is a flag nobody can rely on.
        //
        // there is no `--progress` beside it, and the omission is deliberate. `--color=always` exists
        // because a CI job legitimately renders SGR out of a stored log; nobody wants a carriage return
        // in one. The day that output is wanted the answer is a `plain` mode - committed rows, no cursor
        // movement - and not a force of this one, which would be a loaded gun beside the e2e corpus
        command.get().add_argument("--silent")
            .help("Do not draw the progress checklist. Diagnostics and every explicit dump are unaffected.")
            .default_value(false)
            .implicit_value(true);

    }

    // add IR & AST printing flag
    for (auto &command : {std::ref(run_command), std::ref(build_command)}) {
        command.get().add_argument("-p", "--print-ir")
            .help("Print the LLVM IR to the console.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-a", "--print-ast")
            .help("Print the AST as parsed, before the semantic passes.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-ar", "--print-resolved-ast")
            .help("Print the AST after the semantic passes: derefs, drops, retains and releases included.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-syt", "--print-symbol-table")
            .help("Print the registered symbol table to the console.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-pi", "--print-instances")
            .help("Print the monomorphizer's instances, rewired call sites and struct instantiations.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-O", "--optimize")
            .help("Whole-program: fold every unit into one module and run the O3 pipeline over it. "
                  "Bypasses the module object cache, which per-module objects and a merge cannot both have.")
            .default_value(false)
            .implicit_value(true);

        // **the dump for the path that has no merge.** `-p` folds every unit into one module, because that
        // is the only thing an O3 pipeline or a single dump can look at - so it can only ever show the
        // whole-program build. This one shows each unit as the object writer gets it, which is what an
        // ordinary `echoc build` actually emits, and is the only way to assert on it
        command.get().add_argument("-pu", "--print-unit-ir")
            .help("Print each compilation unit's IR separately, as the object writer receives it. The "
                  "per-module path, where '-p' shows the merged whole-program one.")
            .default_value(false)
            .implicit_value(true);

        // **not the inverse of -O**, and the help text has to say so: -O changes *what is compiled
        // together*, this only turns off the per-unit pipeline every build otherwise gets. For reading raw
        // IR and for bisecting a miscompile against the optimizer
        command.get().add_argument("--no-optimize")
            .help("Emit unoptimized IR: skip the per-unit pipeline an ordinary build runs. Not the inverse "
                  "of -O, which merges every unit and optimizes the whole program.")
            .default_value(false)
            .implicit_value(true);

        // **orthogonal to --debug/--release**, and the help text has to say so: that pair decides which
        // checks the program carries, this decides what the object tells a debugger. On its own it also
        // turns the per-unit pipeline off - see resolve_options - because a line table over O2 output
        // describes a program nobody wrote
        command.get().add_argument("-g", "--debug-info")
            .help("Emit DWARF, so the program can be stepped through in a debugger. Orthogonal to "
                  "--debug/--release, which decides which checks the program carries. Implies "
                  "--no-optimize unless -O is given.")
            .default_value(false)
            .implicit_value(true);

        // **not a tuning knob.** a *defined* program must answer the same with it and without it, so
        // this is how the corpus's adversarial cases are run both ways - and how a miscompile is
        // bisected against the aliasing model rather than against the optimizer
        command.get().add_argument("--no-tbaa")
            .help("Emit no type-based alias metadata. A defined program must produce the same result "
                  "with and without it; a difference is a bug in the family table or a false 'unsafe'.")
            .default_value(false)
            .implicit_value(true);

        // the measurement dumps both subcommands have. They sit with `-a`/`-ar`/`-p`/`-syt`/`-pi` rather than
        // off to one side because they answer the same kind of question those do - what did the compiler
        // actually do - and a number nobody can ask for is a number nobody looks at. Each prints a `[section]`
        // header, the shape --print-symbol-table already uses, so the output is greppable and stays stable
        // enough to assert on. `--explain-prune` above is the third and belongs to `run` alone
        command.get().add_argument("-ec", "--explain-cache")
            .help("Print each module's cache key, whether its artifact is present, and what changed.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-t", "--timings")
            .help("Print where the compile spent its time, by phase.")
            .default_value(false)
            .implicit_value(true);

        // and the fourth, which is unlike the other three in one way worth knowing before you reach for
        // it: `-ec`, `-t` and `-ep` print something echoc worked out, and this one **changes the program
        // that is emitted**. It maintains a counter beside every allocation and has `main` print what is
        // still outstanding on its way out, so the number describes the program's own run rather than the
        // compile. Off by default for exactly that reason
        //
        // no entry in the module cache key: the entry module is never cached, so the report can never be
        // silently missing from a served artifact. --track-allocations *is* in the key, because it
        // changes what every other module's object contains
        command.get().add_argument("-em", "--explain-memory")
            .help("Make the compiled program print how many allocations were still outstanding when it "
                  "ended. Implies --track-allocations.")
            .default_value(false)
            .implicit_value(true);

        // the bookkeeping on its own, for a program that wants to read the count from Echo with
        // `mem::live_allocations()` rather than have one printed at the end
        command.get().add_argument("-ta", "--track-allocations")
            .help("Count how many allocations are outstanding, so 'mem::live_allocations()' can be read.")
            .default_value(false)
            .implicit_value(true);

        // **the pressure valve, and deliberately the same grammar a manifest writes.** Where a library
        // lives is a property of the machine rather than of the module, so there has to be a way to reach
        // one nothing declared - but a *second* spelling for it would be two things to keep correct, and
        // the one used least would be the one that drifts. Merged after every manifest's, so a declaration
        // wins the dedup and a search path given here is consulted last
        command.get().add_argument("--link")
            .help("Add a link requirement: 'lib:<name>', 'framework:<name>', 'search:<dir>' or "
                  "'object:<file>'. May be given more than once.")
            .default_value(std::vector<std::string>{})
            .append();

        // **the other kind of target flag entirely**, and the two are neighbours here only because they
        // start with the same word. The pair above changes what a `#[if:]` condition sees; this pair
        // changes what is emitted - every cost model in the optimizer and instruction selection itself
        // are downstream of it.
        //
        // the default is the *platform baseline for the triple*, never the host: `native` produces an
        // object that may not run on the machine next to it, with an illegal instruction rather than a
        // diagnostic, and `echoc build` hands somebody a binary. So the host is reachable and has to be
        // asked for by name
        command.get().add_argument("--target-cpu")
            .help("Which CPU to optimize and select instructions for. Defaults to the platform "
                  "baseline for this target; 'native' means this machine, whose output may not run "
                  "elsewhere.")
            .default_value(std::string(""));

        command.get().add_argument("--target-features")
            .help("Target features to enable or disable, as '+sve,-crc'. Empty unless asked for.")
            .default_value(std::string(""));

        // the build mode decides which checks the program carries: `assert` and the null check the
        // `ptr<T>` -> `T&` narrowing emits are both debug-only. deliberately orthogonal to -O,
        // which says how hard to optimize what is emitted, not what to emit
        //
        // `run` defaults to debug and `build` to release - see main_run / main_build. the group is
        // what refuses both at once, so the conflict is reported by the parser with usage, the way
        // every other CLI mistake is, rather than by a hand-rolled check with its own message
        auto &build_mode = command.get().add_mutually_exclusive_group();

        build_mode.add_argument("--debug")
            .help("Debug build: keep 'assert' and the compiler's own runtime checks. The default for 'run'.")
            .default_value(false)
            .implicit_value(true);

        build_mode.add_argument("--release")
            .help("Release build: drop 'assert' and the compiler's own runtime checks. The default for 'build'.")
            .default_value(false)
            .implicit_value(true);

        // the standard library is a module like any other, so it can be left out. `die`, `assert`
        // and the stdlib namespaces are then simply undeclared names.
        //
        // grouped with --emit-stdlib-header for the same reason --debug and --release are grouped:
        // there is no stdlib module to write out, so the combination is a CLI mistake and the parser
        // is where a CLI mistake is reported - not `build_bundle`, whose job is to construct modules
        auto &stdlib_use = command.get().add_mutually_exclusive_group();

        stdlib_use.add_argument("--no-stdlib")
            .help("Compile without the standard library. 'array', 'string', 'die', 'assert' and the "
                  "'contract::', 'mem::', 'str::', 'arr::', 'hash::' and 'std::' namespaces are then undeclared.")
            .default_value(false)
            .implicit_value(true);

        // regenerating the embeddable stdlib header rewrites a tracked file, so it is opt-in
        // rather than a side effect of every compile
        stdlib_use.add_argument("--emit-stdlib-header")
            .help("Regenerate stdlib/build/stdlib_embedded.h from the standard library sources.")
            .default_value(false)
            .implicit_value(true);
    }

    cli.add_subparser(run_command);
    cli.add_subparser(build_command);
    cli.add_subparser(clean_command);

    try {
        cli.parse_args(echoc_argc, argv);
    }
    catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << cli;
        return 1;
    }

    // enabled here rather than inside each entry point, so the very first phase a subcommand enters is
    // already being timed
    if (cli.is_subcommand_used(run_command) || cli.is_subcommand_used(build_command)) {
        auto &command = cli.is_subcommand_used(run_command) ? run_command : build_command;

        if (command.get<bool>("--timings")) {
            Compiler::PhaseTimings::instance().enable();
        }
    }

    if (!cli.is_subcommand_used(run_command)
        && !cli.is_subcommand_used(build_command)
        && !cli.is_subcommand_used(clean_command)) {
        std::cerr << cli;
        return 1;
    }

    // whichever one was used, for the flags all three share - see the loop that registers them
    auto &command = cli.is_subcommand_used(run_command)
        ? run_command
        : (cli.is_subcommand_used(build_command) ? build_command : clean_command);

    // **one renderer, built here and passed down.** Everything that reports takes a reference to it, so
    // there is exactly one answer to what a diagnostic looks like - which is the whole point: this
    // replaced three printers that had each grown their own idea of it
    Compiler::ColorChoice color_choice = Compiler::ColorChoice::t_auto;
    Compiler::DiagnosticFormat format = Compiler::DiagnosticFormat::t_auto;
    std::string flag_error;

    if (!Compiler::parse_color_choice(command.get<std::string>("--color"), color_choice, flag_error)
        || !Compiler::parse_diagnostic_format(
            command.get<std::string>("--diagnostics"), format, flag_error)) {
        std::cerr << flag_error << std::endl;
        return 1;
    }

    const Compiler::TerminalCapabilities capabilities
        = Compiler::TerminalCapabilities::resolve(color_choice, format);

    // **stderr, whatever the format.** stdout under `run` belongs to the program being executed, so a
    // compiler writing into it is a compiler whose output cannot be piped - and an editor reading the
    // json form wants one stream that carries diagnostics and nothing else
    const AST::DiagnosticRenderer diagnostics(std::cerr, format, capabilities);

    // **the same stream, and the gate that keeps them from fighting over it.** The checklist is drawn only
    // when stderr is a terminal that can be redrawn, `--silent` was not given, and the format is not the
    // machine-readable one - so a pipe, a CI log and the e2e corpus write nothing and need no
    // configuration, which is the rule `--diagnostics=auto` already follows for the same reason.
    //
    // enabled after the capabilities and not beside PhaseTimings above, because it needs them - and after
    // the renderer, because a row must never be the first thing on a stream a diagnostic is about to fail
    // onto
    if (capabilities.interactive && !command.get<bool>("--silent") && !diagnostics.is_machine_readable()) {
        Compiler::ProgressReporter::instance().enable(std::cerr, capabilities);
    }

    if (cli.is_subcommand_used(run_command)) {
        return main_run(run_command, diagnostics, program_arguments, envp);
    }

    if (cli.is_subcommand_used(clean_command)) {
        return main_clean(clean_command, diagnostics);
    }

    return main_build(build_command, diagnostics);
}
