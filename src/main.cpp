#include <iostream>
#include <fstream>
#include <sstream>

#include <argparse.h>

#include "eco.h"
#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ASTMonomorphizer.h"
#include "AST/ASTPointerAdjuster.h"
#include "AST/ASTTypeChecker.h"
#include "Parser/ManifestParser.h"
#include "Parser/ModuleParser.h"
#include "Compiler/CompilerException.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/LLVM/LLVMCompiler.h"

#if ECO_USE_EMBEDDED_STDLIB
#include "stdlib_embedded.h"
#endif

#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>
#include <chrono>

#define SH_COLOR_RST  "\x1B[0m"
#define SH_COLOR_KRED  "\x1B[31m"
#define SH_COLOR_KGRN  "\x1B[32m"
#define SH_COLOR_KYEL  "\x1B[33m"
#define SH_COLOR_KBLU  "\x1B[34m"
#define SH_COLOR_KMAG  "\x1B[35m"
#define SH_COLOR_KCYN  "\x1B[36m"
#define SH_COLOR_KWHT  "\x1B[37m"

#define SH_COLOR_FRED(x) SH_COLOR_KRED x SH_COLOR_RST
#define SH_COLOR_FGRN(x) SH_COLOR_KGRN x SH_COLOR_RST
#define SH_COLOR_FYEL(x) SH_COLOR_KYEL x SH_COLOR_RST
#define SH_COLOR_FBLU(x) SH_COLOR_KBLU x SH_COLOR_RST
#define SH_COLOR_FMAG(x) SH_COLOR_KMAG x SH_COLOR_RST
#define SH_COLOR_FCYN(x) SH_COLOR_KCYN x SH_COLOR_RST
#define SH_COLOR_FWHT(x) SH_COLOR_KWHT x SH_COLOR_RST

#define SH_COLOR_BOLD(x) "\x1B[1m" x SH_COLOR_RST
#define SH_COLOR_UNDL(x) "\x1B[4m" x SH_COLOR_RST


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

// colour only when somebody is watching. A redirected stream is being read by a program - a golden test,
// a log, a pipe into `grep` - and an escape sequence in it is noise that has to be filtered back out
static bool stdout_is_a_terminal()
{
    static const bool answer = isatty(fileno(stdout)) != 0;
    return answer;
}

void print_critical_error(std::string title, std::string message)
{
    if (stdout_is_a_terminal()) {
        std::cout << SH_COLOR_BOLD(SH_COLOR_FRED( << title <<) ) << std::endl;
    }
    else {
        std::cout << title << std::endl;
    }

    for (size_t i = 0; i < title.size(); i++) {
        std::cout << "-";
    }

    std::cout << std::endl;
    std::cout << message << std::endl;
}

int handle_parse(Parser::ModuleParser &parser, Parser::ModuleParser::InputPayload &input)
{
    // **caught whatever ECO_DONT_CATCH_EXCEPTIONS says**, unlike the tokenization error inside. That macro
    // exists to let a *compiler bug* crash with a stack trace, and a malformed `#[if: ...]` is not one - it
    // is a mistake in the source being compiled, and reporting it as a crash would blame echoc for it
    try {
        parser.parse_input(input);
    }
    catch (AST::Module::TokenFilterException &e) {
        print_critical_error("Conditional Compilation Failed", e.what());
        return 1;
    }
#if !ECO_DONT_CATCH_EXCEPTIONS
    catch (Parser::ModuleParser::TokenizationException &e) {
        print_critical_error("Tokenization Failed", e.what());
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
    const std::filesystem::path candidate = std::filesystem::current_path(ec) / "module.eco";

    if (ec || !std::filesystem::is_regular_file(candidate, ec)) {
        return std::nullopt;
    }

    return candidate;
}

// the manifests this invocation builds, in the order the modules have to be parsed: every `-m` the user
// gave, plus the standard library's own, which is an ordinary manifest module like any other
//
// resolving the whole graph here rather than per flag is what makes a dependency implicit: naming a
// library that depends on another pulls the other in, once, in the right order
static bool resolve_manifests(
    argparse::ArgumentParser &cli,
    const Compiler::TargetFacts &facts,
    std::vector<Parser::ModuleManifest> &out,
    std::vector<std::filesystem::path> &out_roots)
{
    std::vector<std::filesystem::path> roots;

#if !ECO_USE_EMBEDDED_STDLIB
    if (!cli.get<bool>("--no-stdlib")) {
        roots.push_back(std::filesystem::path(STDLIB_SOURCE_DIR) / "module.eco");
    }
#endif

    // the standard library is not one of *the user's* roots - it is added to every build - so it is
    // deliberately not in out_roots. Whoever asks "what did this invocation point at" must not be told
    // "the standard library"
    const size_t implicit_roots = roots.size();

    for (const auto &named : cli.get<std::vector<std::string>>("--module")) {
        roots.push_back(std::filesystem::path(named));
    }

    if (roots.size() == implicit_roots && cli.get<std::vector<std::string>>("source").empty()) {
        if (const std::optional<std::filesystem::path> discovered = discover_project_manifest()) {
            roots.push_back(discovered.value());
        }
    }

    out_roots.assign(roots.begin() + implicit_roots, roots.end());

    if (roots.empty()) {
        return true;
    }

    std::string error;
    if (!Parser::resolve_module_graph(roots, facts, out, error)) {
        print_critical_error("Module Manifest Error", error);
        return false;
    }

    return true;
}

// one AST::Module per manifest, parsed completely before the next one starts - which is what the
// topological order above exists for
static int parse_manifest_modules(
    const std::vector<Parser::ModuleManifest> &manifests,
    AST::Bundle &bundle,
    Parser::ModuleParser &parser)
{
    for (const Parser::ModuleManifest &manifest : manifests) {
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

        if (handle_parse(parser, input)) {
            return 1;
        }
    }

    return 0;
}

// builds the bundle both `run` and `build` compile: the stdlib module, then the main module with
// the user's sources. one function rather than two copies, because the copies had already drifted
// - `build` never created a stdlib module at all, so any program calling `mem::` or `std::math::`
// compiled under `run` and failed under `build`
static int build_bundle(
    argparse::ArgumentParser &cli,
    AST::Bundle &bundle,
    Parser::ModuleParser &parser,
    std::vector<Parser::ModuleManifest> &out_manifests,
    std::string &out_entry_module)
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
        if (!resolve_manifests(cli, parser.target_facts, manifests, roots)) {
            return 1;
        }
    }

    if (parse_manifest_modules(manifests, bundle, parser) != 0) {
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
        // decides what its module is called
        const std::filesystem::path &root = roots.front();
        auto found = std::find_if(manifests.begin(), manifests.end(),
            [&root](const Parser::ModuleManifest &manifest) {
                std::error_code ec;
                return std::filesystem::equivalent(manifest.path, root, ec);
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

        if (handle_parse(parser, input)) {
            return 1;
        }
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
    argparse::ArgumentParser &cli, AST::Bundle &bundle, const Compiler::CompilerOptions &options)
{
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
        std::cout << monomorphizer.debug_dump_instances() << std::endl;
    }

    // semantic analysis on the concrete AST: resolves member accesses and call arguments and
    // records located issues, so type errors surface here instead of deep in codegen
    // make the pointer transparency the language promises explicit in the tree: every pointer
    // read in a value position gains a deref node, so from here on result_type() is honest
    AST::PointerAdjuster(bundle).run();

    // the one pass that reads the options: a builtin can be unavailable, and only the command line
    // knows whether this one is
    AST::TypeChecker(bundle, options).run();

    print_resolved_ast(cli, bundle);

    bundle.collector.print_issues();
    if (bundle.collector.has_critical_issues()) {
        std::cout << "Critical issues found, cannot compile." << std::endl;
        return 1;
    }

    return 0;
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
    argparse::ArgumentParser &cli, Compiler::BuildMode fallback)
{
    Compiler::CompilerOptions options;

    options.mode = fallback;

    if (cli.get<bool>("--debug")) {
        options.mode = Compiler::BuildMode::t_debug;
    }
    else if (cli.get<bool>("--release")) {
        options.mode = Compiler::BuildMode::t_release;
    }

    options.report_allocations = cli.get<bool>("--explain-memory");
    options.track_allocations = options.report_allocations || cli.get<bool>("--track-allocations");

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

// decides, per manifest module, whether its object can be reused. Reads the store; writes nothing
static ModulePlan plan_module_artifacts(
    argparse::ArgumentParser &cli,
    const std::vector<Parser::ModuleManifest> &manifests,
    const std::map<std::string, Compiler::ModuleCacheKey> &keys,
    const std::string &entry_module)
{
    ModulePlan plan;

    const std::filesystem::path cache_dir_override(cli.get<std::string>("--cache-dir"));

    for (const Parser::ModuleManifest &manifest : manifests) {
        if (manifest.name == entry_module) {
            continue;
        }

        auto found = keys.find(manifest.name);
        if (found == keys.end()) {
            continue;
        }

        const std::filesystem::path object =
            Compiler::module_object_path(manifest, found->second, cache_dir_override);

        std::error_code ec;
        if (std::filesystem::is_regular_file(object, ec)) {
            plan.cached.insert(manifest.name);
            plan.reused.push_back(object);
            continue;
        }

        // **an unwritable store is not an error.** A read-only library directory, a toolchain installed
        // system-wide, a full disk: the module is compiled to a scratch object beside the executable and simply
        // not kept. Failing the build over a missed optimization would make a cache a liability
        if (!Compiler::cache_dir_is_writable(object.parent_path())) {
            continue;
        }

        plan.emit_to[manifest.name] = ModuleArtifact{
            object, Compiler::module_inputs_path(manifest, cache_dir_override) };
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
    const std::vector<Parser::ModuleManifest> &manifests,
    const Compiler::CompilerOptions &options,
    const Compiler::TargetFacts &facts,
    std::map<std::string, Compiler::ModuleCacheKey> &out_keys)
{
    Compiler::ScopedPhase phase("cache keys");

    std::string error;
    if (!Compiler::compute_module_keys(
            manifests, options, facts, cli.get<bool>("--optimize"), out_keys, error)) {
        print_critical_error("Module Cache Error", error);
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
    bool bypassed)
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
    LLVMCompiler &compiler, const std::string &output, const ModulePlan &plan)
{
    std::vector<std::filesystem::path> objects = plan.reused;

    // a module with nowhere to be stored - the entry module, or anything not from a manifest - gets a scratch
    // object beside the executable, the same place the whole-program path has always put one
    const auto object_for = [&](const std::string &module_name) -> std::filesystem::path {
        auto found = plan.emit_to.find(module_name);
        if (found != plan.emit_to.end()) {
            return found->second.object;
        }

        return std::filesystem::path(output + "." + module_name + ".o");
    };

    if (!compiler.emit_objects(object_for, objects)) {
        return false;
    }

    return compiler.link_executable(output, objects);
}

// the inputs each freshly emitted module was built from, so the next miss can name what changed.
//
// best effort: a store that cannot be written is a cache that will miss next time, which is slow rather than
// wrong. Refusing the build over it would make an unwritable directory fatal to compiling
static void store_module_records(
    const ModulePlan &plan, const std::map<std::string, Compiler::ModuleCacheKey> &keys)
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
static int report_compiler_exception(const Compiler::ASTCompilerException &e)
{
    std::cout << "Compiler Exception: " << e.what() << std::endl;

    // the same renderer Collector::print_issues uses, so the two cannot drift on how an issue reads
    AST::print_issue(e.issue());

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

    // empty unless something downstream reads one - see needs_cache_keys
    std::map<std::string, Compiler::ModuleCacheKey> cache_keys;
};

// keying a module reads every one of its sources, so it is not free. Only two things ever look at a key: the
// plan, which a whole-program build does not have, and `--explain-cache`
static bool needs_cache_keys(argparse::ArgumentParser &cli, bool whole_program)
{
    return !whole_program || cli.get<bool>("--explain-cache");
}

static bool run_front_end(
    argparse::ArgumentParser &cli,
    AST::Bundle &bundle,
    Compiler::BuildMode fallback,
    bool whole_program,
    FrontEnd &out)
{
    // **before the parse**, and that is not a preference: the conditional filter runs between lexing and
    // pass 1, so what a condition sees has to be settled before a single file is read. Everything here
    // comes off the command line, so there is nothing to wait for
    {
        std::string error;

        if (!Compiler::TargetFacts::resolve(
                cli.present("--target-os") ? cli.get<std::string>("--target-os") : std::string(),
                cli.present("--target-arch") ? cli.get<std::string>("--target-arch") : std::string(),
                cli.get<std::vector<std::string>>("--define"),
                out.target_facts,
                error)) {
            print_critical_error("Invalid Target", error);
            return false;
        }
    }

    // **after the facts and not before**, which is why the parser is built here rather than handed in: it
    // takes them at construction, so there is no window in which one exists that has not been told what
    // platform it is reading for. Neither subcommand touches it once the front end is done
    Parser::ModuleParser parser(out.target_facts);

    {
        Compiler::ScopedPhase phase("parse");
        if (build_bundle(cli, bundle, parser, out.manifests, out.entry_module) != 0) {
            return false;
        }
    }

    // ahead of the semantic passes, because one of them reads it: AST::TypeChecker refuses
    // `mem::live_allocations()` when nothing is counting. it depends on nothing but the command line, so
    // the only thing its old position bought was the appearance of an order
    out.options = resolve_options(cli, fallback);

    {
        Compiler::ScopedPhase phase("semantic passes");
        if (run_semantic_passes(cli, bundle, out.options) != 0) {
            return false;
        }
    }

    if (needs_cache_keys(cli, whole_program)
        && !compute_cache_keys(cli, out.manifests, out.options, out.target_facts, out.cache_keys)) {
        return false;
    }

    return true;
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

int main_run(
    argparse::ArgumentParser &cli,
    const std::vector<std::string> &program_arguments,
    const char *const *environment)
{
    auto bundle = AST::Bundle();

    // `run` reuses nothing: the JIT is handed one module, so every unit is merged and there are no per-module
    // objects to store or load. Feeding it stored objects instead would mean handing the JIT one per cached
    // module beside main's, which is a question about duplicate weak symbols rather than about caching
    FrontEnd front;
    if (!run_front_end(cli, bundle, Compiler::BuildMode::t_debug, /*whole_program=*/true, front)) {
        return 1;
    }

    const Compiler::CompilerOptions options = front.options;
    const std::string &entry_module = front.entry_module;

    report_cache_plan(cli, front.manifests, front.cache_keys, ModulePlan{}, entry_module, /*bypassed=*/true);

    LLVMCompiler compiler(options);
    compiler.set_entry_module(entry_module);

    try {
        Compiler::ScopedPhase phase("codegen");
        compiler.compile_bundle(bundle);

        // the JIT can only be handed one module, so `run` always merges
        compiler.link_into_main();
    } catch (Compiler::ASTCompilerException &e) {
        return report_compiler_exception(e);
    }

    if (cli.get<bool>("--optimize")) {
        Compiler::ScopedPhase phase("optimize");
        compiler.optimize();
    }

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

    // the program's own status, so `echoc run` exits the way it did. Today that is always 0 on this
    // path - the entry point's epilogue returns 0 and every other ending goes through libc's `exit`
    // from inside the JIT, which never comes back here at all
    return status;
}

int main_build(argparse::ArgumentParser &cli)
{
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
    if (!run_front_end(cli, bundle, Compiler::BuildMode::t_release, whole_program, front)) {
        return 1;
    }

    const Compiler::CompilerOptions options = front.options;
    const std::string &entry_module = front.entry_module;

    // an optimized or dumped build reuses nothing and stores nothing - see wants_whole_program_module
    const ModulePlan plan = whole_program
        ? ModulePlan{}
        : plan_module_artifacts(cli, front.manifests, front.cache_keys, entry_module);

    report_cache_plan(cli, front.manifests, front.cache_keys, plan, entry_module, whole_program);

    LLVMCompiler compiler(options);
    compiler.set_entry_module(entry_module);

    try {
        Compiler::ScopedPhase phase("codegen");
        compiler.compile_bundle(bundle, plan.cached);

        if (whole_program) {
            compiler.link_into_main();
        }
    } catch (Compiler::ASTCompilerException &e) {
        return report_compiler_exception(e);
    }

    // read off the flag, exactly as `run` does. this used to be unconditional, which made `-O` a
    // switch `build` accepted and silently ignored - and left no way at all to see what codegen
    // emitted for a release build, since `-p` only ever showed the optimizer's output
    if (cli.get<bool>("--optimize")) {
        Compiler::ScopedPhase phase("optimize");
        compiler.optimize();
    }

    if (cli.get<bool>("--print-ir")) {
        compiler.printIR(false);
    }

    const std::string output = cli.get<std::string>("-o");

    {
        Compiler::ScopedPhase phase("emit + link");

        if (whole_program) {
            // one merged module, one object, one link - the path that has to keep existing because `-O`
            // depends on it
            if (!compiler.make_exec(output)) {
                return 1;
            }
        }
        else if (!emit_and_link_modules(compiler, output, plan)) {
            return 1;
        }
    }

    // only now, and only for what was actually emitted: a record written before the object exists would
    // describe a build that may still have failed
    if (!whole_program) {
        store_module_records(plan, front.cache_keys);
    }

    std::cout << Compiler::PhaseTimings::instance().report();

    return 0;
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

    argparse::ArgumentParser cli("echoc");
    cli.add_description("The echo programming language compiler");

    argparse::ArgumentParser run_command("run");
    run_command.add_description("Runs the given source files.");
    run_command.add_argument("source")
        .default_value(std::vector<std::string>{})
        .help(".eco source files to be parsed, compiled and run.")
        .remaining();
    
    argparse::ArgumentParser build_command("build");
    build_command.add_description("Builds the given source files.");
    build_command.add_argument("source")
        .default_value(std::vector<std::string>{})
        .help(".eco source files to be parsed and compiled.")
        .remaining();
    
    build_command.add_argument("-o", "--output")
        .help("Output file name.");

    // the third measurement dump, and the only one not shared: `build` never prunes, because the prune is
    // part of the JIT and a per-module object may not depend on what reaches `main`. A flag a subcommand
    // accepts and silently ignores is worse than one it rejects - `-O` was exactly that on `build` - so
    // this is registered where it means something and nowhere else. `-o` above is the same asymmetry the
    // other way round
    run_command.add_argument("-ep", "--explain-prune")
        .help("Print what the JIT prune dropped, and what the entry point still reaches.")
        .default_value(false)
        .implicit_value(true);

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
            .help("Sets the optimization level to 3, makes your code go brrrrrr.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("--cache-dir")
            .help("Where compiled module artifacts are stored. Defaults to '.echo' beside each manifest.")
            .default_value(std::string(""));

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

        // repeatable, and the graph is resolved as a whole - so naming a library that depends on another
        // pulls the other in too, once, ahead of it. A dependency does not have to be spelled here
        command.get().add_argument("-m", "--module")
            .help("Build a module from its manifest. May be given more than once; a manifest may be an "
                  "'module.eco' file or a directory holding one.")
            .default_value(std::vector<std::string>{})
            .append()
            .metavar("MANIFEST");

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
                  "'contract::', 'mem::', 'str::', 'arr::' and 'std::' namespaces are then undeclared.")
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

    if (cli.is_subcommand_used(run_command)) {
        return main_run(run_command, program_arguments, envp);
    }
    else if (cli.is_subcommand_used(build_command)) {
        return main_build(build_command);
    }
    else {
        std::cerr << cli;
        return 1;
    }
}