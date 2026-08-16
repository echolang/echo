#include <iostream>
#include <fstream>
#include <sstream>

#include <fmt/core.h>

#include "eco.h"
#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFileRoot.h"
#include "AST/ASTSourceToken.h"
#include "AST/ASTDiagnosticRenderer.h"
#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ASTMonomorphizer.h"
#include "AST/ASTAccessPass.h"
#include "AST/ASTPointerAdjuster.h"
#include "AST/ASTTypeChecker.h"
#include "AST/ASTMangler.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ManifestParser.h"
#include "Parser/ModuleParser.h"
#include "Compiler/BuildLayout.h"
#include "Compiler/CBuild.h"
#include "Compiler/CommandLine.h"
#include "Compiler/CommandLineHelp.h"
#include "Compiler/CommandLineOption.h"
#include "Compiler/DriverOptions.h"
#include "Compiler/CompilerException.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/ProgressReporter.h"
#include "Compiler/SettledPath.h"
#include "Compiler/TargetSubtarget.h"
#include "Compiler/TestReporter.h"
#include "Compiler/TestRunner.h"
#include "Compiler/TestSelection.h"
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
    const std::vector<std::string> &path_strings, size_t &missing)
{
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
    // is a mistake in the source being compiled, and reporting it as a crash would blame echoc for it.
    //
    // the banner names conditional compilation and covers a malformed `test` header too, that being the
    // other thing Parser::filter_conditional_tokens decides: a test block is a region compiled under one
    // condition, and the filter has to read its header to know where the region it is dropping ends
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
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    Compiler::TargetFacts &out
)
{
    std::string error;

    if (!Compiler::TargetFacts::resolve(
            driver.target_os,
            driver.target_arch,
            driver.defines,
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
    const std::filesystem::path &package_dir_override,
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

    Parser::ManifestScratch scratch(facts);

    // one package directory for the whole invocation. a vendored module's own `#[requires:]`
    // resolve here too, so the tree stays flat
    if (!out_roots.empty()) {
        scratch.package_dir = Parser::resolve_package_dir(
            out_roots.front().parent_path(), package_dir_override);
    }
    else if (!package_dir_override.empty()) {
        scratch.package_dir = Parser::resolve_package_dir({}, package_dir_override);
    }

    if (!Parser::resolve_module_graph(roots, scratch, out)) {
        scratch.bundle.collector.print_issues(diagnostics);
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
    const std::vector<const Parser::ModuleManifest *> &manifests,
    const std::vector<std::filesystem::path> &roots,
    const Parser::ActiveTargets &active_targets,
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

        // **through the one owner, never off `manifest.sources`.** What this module compiles is a question
        // about the program being built, and the module cache asks the very same function - two merges
        // here would be a cache handing one program the object built for another
        const Parser::ModuleContribution contribution =
            Parser::module_contribution_for(manifest, active_targets);

        for (const auto &source : contribution.sources) {
            input.files.push_back(Parser::ModuleParser::InputFile(source));
        }

        step.summary(fmt::format(
            "{} file{}",
            contribution.sources.size(), contribution.sources.size() == 1 ? "" : "s"));

        // **a module the invocation pointed at lists its files; a dependency reports a count.** the same
        // distinction resolve_manifests already draws, and for the sentence it draws it with: whoever
        // asks what this invocation pointed at must not be told "the standard library". Twenty-one rows
        // of stdlib under every build is that answer given anyway
        if (manifest_is_a_root(manifest, roots)) {
            step.detail(contribution.sources);
        }

        if (handle_parse(diagnostics, parser, input)) {
            return 1;
        }

        step.finish(true);
    }

    return 0;
}

// one program this invocation produces.
//
// **there is one of these whether or not a target was declared.** A module declaring no `#[target:]` is
// one program whose entry is every file root of it and whose path came from `-o` - which is what every
// program compiled before targets existed was - so nothing downstream has an arm for "no targets"
struct Program
{
    // the declaring target's name, empty when no target declared this program. what the checklist calls
    // it, and what `argv[0]` reads under `run`
    std::string name;

    std::string entry_module;

    // the one file of that module whose root is the program. **empty means every file root of it**, and
    // that is the shape a target-less program has always had
    std::filesystem::path entry_file;

    std::filesystem::path output;

    // whose `#[target: ...] { ... }` scopes this program opens. **Deliberately not derivable from `name`**:
    // a test run has no target name at all and still opens every test target of every module it was
    // pointed at, and a target-less build opens nothing while naming nothing either.
    //
    // read only through Parser::module_contribution_for, which is the one owner of what a module compiles
    Parser::ActiveTargets active_targets;
};

// what an invocation settles once, before any program is compiled.
//
// **the split is the whole of what a target costs.** Which modules there are, what platform they are read
// for, and where their artifacts go are facts about the *invocation*; parsing them and generating code is
// work done per *program*. Resolving the module graph a second time to find out what a manifest declares
// would be a second answer to a question that already has an owner, so it is resolved here, once, and
// handed down to every program built from it
struct Invocation
{
    Compiler::TargetFacts target_facts;

    // **every manifest this project reaches**, in dependency order - which, since a `#[target: ...] { }`
    // scope may declare a `#[depends:]`, is no longer the same set as the modules a given program
    // compiles. FrontEnd::manifests() is that narrower answer, and carries a different type so the two
    // cannot be mistaken for one another
    std::vector<Parser::ModuleManifest> manifests;

    // the user's roots only, never the standard library - this is what "which module is the program" is
    // asked of, and being told "the standard library" would answer it wrongly
    std::vector<std::filesystem::path> roots;

    // the loose `.eco` files named on the command line, expanded
    std::vector<std::filesystem::path> sources;

    Compiler::BuildLayout layout;

    // the modules whose `test` blocks this invocation compiles, by name. **empty for everything but
    // `echoc test`**, so a normal build's tokens never reach a parser that would keep one.
    //
    // settled here beside `roots` because it is derived from them and from nothing else: an invocation
    // compiles the tests of the modules it pointed at, which is the same separation `roots` exists for
    std::set<std::string> test_modules;

    // the `#[target: test]`s this invocation selected, empty when the entry manifest declares none or when
    // this is not a test invocation.
    //
    // **beside `programs` rather than inside one**, because a test target is not a program: a test run
    // compiles the module exactly once whichever targets were named, and what a target contributes is a
    // *selection* over the tests that come out. Several of them therefore mean one compile and a union,
    // where several `exe` targets mean several builds
    std::vector<Parser::ModuleTarget> test_targets;

    std::vector<Program> programs;
};

// **does this manifest name a file of its own as a program?**
//
// The *executable* targets and not every target, which is the whole of the question: a `#[target: test]`
// produces no artifact and names no entry, so a module declaring only those still is the one program its
// module has always been - every file root of it, concatenated. Two readers, and they were two copies of
// this predicate 900 lines apart before it had a name: resolve_programs, deciding whether there is a target
// to build, and collect_shared_top_level_code, deciding whether "only an entry's top level runs" applies
static bool module_declares_a_program(const Parser::ModuleManifest &manifest)
{
    return std::any_of(
        manifest.targets.begin(), manifest.targets.end(),
        [](const Parser::ModuleTarget &target) {
            return target.kind == Parser::TargetKind::t_executable;
        });
}

// **top-level code in a file no target claims**, collected once per file.
//
// only asked of a module that declares targets, because that is the only module with anything on record
// saying which of its files are programs. A file that *is* some other target's entry is left alone: its
// code belongs to that program and not being part of this one is the point of declaring both.
//
// **collects and returns, never renders.** the gate at the end of run_semantic_passes is the one that
// turns a full collector into output and an exit status - it flushes stdout ahead of stderr first, which
// a second gate here got wrong, and it reports every issue under one summary rather than this file's
// under one and the rest under another
static void collect_shared_top_level_code(const Parser::ModuleManifest &manifest, AST::Bundle &bundle)
{
    // **asked of the programs, not of the targets.** The rule this function states is "a target's entry
    // file becomes the program, so a shared file's top level never runs", and that only holds once
    // something claims an entry. A test target claims none by construction, so a module declaring only
    // those still gets the program its module has always been - and until this asked the right question
    // every file of such a module holding top-level code was refused against a rule nobody wrote
    if (!module_declares_a_program(manifest)) {
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

        // **a file an active scope contributed is that target's own**, not code shared with the rest of
        // the module, so this rule does not reach it. Answered by subtraction rather than carried: every
        // file here came from the module's contribution, which is `manifest.sources` plus what the scopes
        // added, and `manifest.sources` is sorted - so "not one of the module's own" is one lookup
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

        // **null is a real answer from source_token_of**, and a statement it cannot place is one this
        // cannot underline. Refusing it with no location at all would be worse than the silent drop it
        // replaced, so the file keeps its old behaviour in the one case there is nothing to point at
        if (token == nullptr) {
            continue;
        }

        // through the collector rather than through the manifest's own `<file>:<line>:` channel: this is
        // a mistake in a *source* file, so it gets the underline and the note every other one gets
        bundle.collector.collect_issue<AST::Issue::TopLevelCodeOutsideEntry>(
            AST::CodeRef{ module, token->make_slice() },
            manifest.name,
            file.get_path().filename().string());
    }
}

// builds the bundle both `run` and `build` compile: the stdlib module, the manifest modules, then the
// main module with the user's sources. one function rather than two copies, because the copies had
// already drifted - `build` never created a stdlib module at all, so any program calling `mem::` or
// `std::math::` compiled under `run` and failed under `build`
//
// **the manifests arrive resolved.** Which modules there are is an invocation-wide fact and this runs once
// per program - see Invocation
static int build_bundle(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Invocation &invocation,
    const Program &program,
    const std::vector<const Parser::ModuleManifest *> &manifests,
    AST::Bundle &bundle,
    Parser::ModuleParser &parser
)
{
#if ECO_USE_EMBEDDED_STDLIB
    if (!driver.no_stdlib) {
        parse_embedded_stdlib_module(bundle, parser);
    }
#endif

    // the manifest modules first, in dependency order, and the loose sources after them - so a program on
    // the command line can name anything a manifest declared, and no manifest can name it back. That is
    // the same one-way rule that holds between two manifests, for the same reason
    const std::vector<std::filesystem::path> &source_files = invocation.sources;

    if (parse_manifest_modules(
            diagnostics, manifests, invocation.roots, program.active_targets, bundle, parser) != 0) {
        return 1;
    }

    for (const Parser::ModuleManifest *manifest : manifests) {
        collect_shared_top_level_code(*manifest, bundle);
    }

    if (!source_files.empty()) {
        const bool manifest_is_the_program = std::any_of(
            manifests.begin(), manifests.end(),
            [](const Parser::ModuleManifest *manifest) { return manifest->name == ECO_MAIN_MODULE_NAME; });

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

    // regenerating the embeddable header is a build step, not a compile step. running it on
    // every `echoc run` would rewrite a tracked file as a side effect of compiling
    if (driver.emit_stdlib_header) {
        if (AST::Module *stdlib = bundle.modules.find_module_ptr("stdlib")) {
            AST::write_embedded_module(*stdlib, STDLIB_SOURCE_DIR "/build/stdlib_embedded.h");
        }
        else {
            std::cerr << "--emit-stdlib-header needs the standard library in the build." << std::endl;
            return 1;
        }
    }

    if (driver.prints(Compiler::PrintKind::t_symbols)) {
        // two stores, deliberately: the namespace tree holds the types, the function registry
        // holds the overload sets
        std::cout << bundle.collector.namespaces.root().debug_dump_symbols() << std::endl;
        std::cout << "[functions]" << std::endl;
        std::cout << bundle.collector.functions.debug_dump() << std::endl;
    }

    if (driver.prints(Compiler::PrintKind::t_ast)) {
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
static void print_resolved_ast(const Compiler::DriverOptions &driver, AST::Bundle &bundle)
{
    if (!driver.prints(Compiler::PrintKind::t_ast_resolved)) {
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
    const Compiler::DriverOptions &driver,
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

    if (driver.prints(Compiler::PrintKind::t_instances)) {
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

    print_resolved_ast(driver, bundle);

    // **stdout is flushed first, always.** Diagnostics go to stderr and a JIT'd program's output goes to
    // stdout; the two are unbuffered and buffered respectively, so a reader that merged them would see
    // them out of order without this
    std::cout.flush();

    bundle.collector.print_issues(diagnostics);

    diagnostics.render_summary(
        bundle.collector.error_count(), bundle.collector.warning_count(), compiled);

    return compiled ? 0 : 1;
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
    const std::vector<const Parser::ModuleManifest *> &manifests,
    const std::map<std::string, Compiler::ModuleCacheKey> &keys,
    const std::string &entry_module
)
{
    ModulePlan plan;

    for (const Parser::ModuleManifest *entry : manifests) {
        const Parser::ModuleManifest &manifest = *entry;
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
// the cache key of every manifest module. Computed whenever anything downstream reads one - the plan, or
// `--explain-cache` - and skipped entirely otherwise, because a key costs a read of every source in the
// build and a whole-program build reuses nothing
static bool compute_cache_keys(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const std::vector<const Parser::ModuleManifest *> &manifests,
    const Compiler::CompilerOptions &options,
    const Compiler::TargetFacts &facts,
    const std::set<std::string> &test_modules,
    const Parser::ActiveTargets &active_targets,
    std::map<std::string, Compiler::ModuleCacheKey> &out_keys
)
{
    Compiler::ScopedPhase phase("cache keys");

    std::string error;
    if (!Compiler::compute_module_keys(
            manifests, options, facts, test_modules, active_targets,
            driver.optimize_is_whole_program(), out_keys, error)) {
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
    const Compiler::DriverOptions &driver,
    const std::vector<const Parser::ModuleManifest *> &manifests,
    const std::map<std::string, Compiler::ModuleCacheKey> &keys,
    const ModulePlan &plan,
    const std::string &entry_module_name,
    bool bypassed
)
{
    if (!driver.explains(Compiler::ExplainKind::t_cache)) {
        return;
    }

    std::cout << "[cache]" << std::endl;

    if (bypassed) {
        std::cout << "  bypassed: an optimized or dumped build is whole-program, so no module object is "
                     "reusable" << std::endl;
    }

    for (const Parser::ModuleManifest *entry : manifests) {
        const Parser::ModuleManifest &manifest = *entry;

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
    // scratch object. **inside the build directory and not beside the executable**: an
    // `out.main.o` nobody asked for, that no command removes, and that every project's
    // .gitignore would have to know about
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
// printing the diagnostic and then carrying on would report success on a failed compile: the
// exit code would say 0 while MCJIT ran over a half-built module, or the emit path would write
// objects for one that may be null or only partly linked. the e2e corpus' `expect:` asserts it
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
    // **borrowed, not copied.** Which modules there are, what platform they were read for and where
    // artifacts go are the invocation's answers, settled before the first program is compiled - see
    // Invocation. Holding them again here made two live answers to each, and a build of several targets
    // deep-copied the whole manifest graph once per target to do it. The Invocation outlives every
    // FrontEnd built from it, both being locals of the subcommand
    const Invocation *invocation = nullptr;

    // which of that invocation's programs this front end is for. **the only thing here that differs
    // between two of them**, which is why it is a pointer rather than five copied fields: a per-program
    // fact forgotten on the way in is one that silently reads as the previous program's
    const Program *program = nullptr;

    Compiler::CompilerOptions options;

    // empty unless something downstream reads one - see needs_cache_keys
    std::map<std::string, Compiler::ModuleCacheKey> cache_keys;

    // **the modules this program compiles, in dependency order** - which is not every module the project
    // has: a dependency reached only through some other target's `#[target: ...] { #[depends:] }` is not
    // one of them. Filled once, in run_front_end.
    //
    // pointers into the invocation's list rather than copies, and a *different type* from that list
    // deliberately: a reader that means "everything this project reaches" and one that means "what this
    // program is made of" are two questions, and the compiler is what keeps a new reader from picking the
    // wrong one by writing the name it half-remembered
    std::vector<const Parser::ModuleManifest *> compiled;

    // spellings over the two above, so a reader says what it wants rather than which carrier holds it
    const std::vector<const Parser::ModuleManifest *> &manifests() const { return compiled; }
    const Compiler::TargetFacts &target_facts() const { return invocation->target_facts; }
    const std::set<std::string> &test_modules() const { return invocation->test_modules; }
    const Compiler::BuildLayout &layout() const { return invocation->layout; }
    const std::string &entry_module() const { return program->entry_module; }
    const std::filesystem::path &entry_file() const { return program->entry_file; }

    // what one module contributes to *this* program - its sources, dependencies, link line and C build,
    // with whatever scopes this program opened folded in. The one owner of that question, asked here
    // rather than each reader picking a field off the manifest
    Parser::ModuleContribution contribution(const Parser::ModuleManifest &manifest) const {
        return Parser::module_contribution_for(manifest, program->active_targets);
    }
};

// which of the project's modules this program is made of, in the order they were resolved.
//
// **a module the walk cannot reach is not compiled at all**, which is the whole of what a scoped
// `#[depends:]` buys: a test-only dependency is parsed, keyed and linked by `echoc test` and is absent
// from `echoc build` entirely. Reachability is asked of Parser::module_contribution_for, so what a scope
// contributes is one answer here as it is everywhere else.
//
// **a reachability walk, and the deny-list is only what seeds it.**
//
// Seeding on the roots alone would be wrong: the standard library is added to every build and is
// deliberately *not* one of the user's roots, and neither is a module a `-m` named. So the seed is asked
// the other way round - a module no target's scope anywhere declares is part of every program this project
// produces, exactly as it always was, and is in from the start.
//
// From there it has to be a **closure**, not one step. A module a scope names may be needed by another
// module a scope names: dropping it because no *unconditional* manifest reaches it drops something the
// build genuinely compiles, and the failure is an unresolved symbol in a module whose own manifest looks
// fine. The list is dependency-first, so walking it backwards settles the closure in one pass - everything
// that could name a module is decided before the module is
static std::vector<const Parser::ModuleManifest *> compiled_manifests(
    const Invocation &invocation,
    const Program &program
)
{
    std::set<std::filesystem::path> reached;

    for (const Parser::ModuleManifest &manifest : invocation.manifests) {
        for (const Parser::ModuleTarget &target : manifest.targets) {
            reached.insert(target.depends.begin(), target.depends.end());
        }
    }

    std::vector<const Parser::ModuleManifest *> out;

    // `reached` holds the conditional ones until here, where it becomes its complement plus whatever the
    // walk adds - which is the set this function is actually computing.
    //
    // no fast path for "nothing is conditional", deliberately: an empty `conditional` makes the
    // complement the whole list and the walk can only add what is already in it, so the general path
    // already answers that case with the same set in the same order - and a second path a reader has to
    // prove equivalent is the one that eventually stops being
    std::set<std::filesystem::path> conditional;
    conditional.swap(reached);

    for (const Parser::ModuleManifest &manifest : invocation.manifests) {
        if (conditional.find(manifest.path) == conditional.end()) {
            reached.insert(manifest.path);
        }
    }

    std::vector<std::filesystem::path> depends;

    for (auto manifest = invocation.manifests.rbegin();
            manifest != invocation.manifests.rend(); ++manifest) {
        if (reached.find(manifest->path) == reached.end()) {
            continue;
        }

        depends.clear();
        Parser::append_active_depends(*manifest, program.active_targets, depends);
        reached.insert(depends.begin(), depends.end());
    }

    for (const Parser::ModuleManifest &manifest : invocation.manifests) {
        if (reached.find(manifest.path) != reached.end()) {
            out.push_back(&manifest);
        }
    }

    return out;
}

// where an artifact goes when the program is a loose .eco file rather than a project: there is no manifest
// to put one beside, so nothing here is worth keeping and nothing should be left behind. Per process, so
// two builds running at once do not share a directory one of them is going to remove
static std::filesystem::path fallback_scratch_dir()
{
    return std::filesystem::temp_directory_path() / "echoc" / std::to_string(getpid());
}

// keying a module reads every one of its sources, so it is not free. Only two things ever look at a key: the
// plan, which a whole-program build does not have, and `--explain-cache`
static bool needs_cache_keys(const Compiler::DriverOptions &driver)
{
    return !driver.whole_program || driver.explains(Compiler::ExplainKind::t_cache);
}

// what a refusal quotes back when it has to name a set, joined the one way.
//
// four refusals here name one - the targets a `--target` could have meant, the programs a `run` was handed
// several of, the tests a `--filter` matched none of - and they are the ones a user compares side by side,
// so they read alike by sharing this rather than by each being got right
template <typename T, typename Spell>
static std::string comma_list(const std::vector<T> &items, Spell spell)
{
    std::string list;

    for (const T &item : items) {
        if (!list.empty()) {
            list += ", ";
        }

        list += spell(item);
    }

    return list;
}

static std::string target_name_list(const std::vector<Parser::ModuleTarget> &targets)
{
    return comma_list(targets, [](const Parser::ModuleTarget &target) { return target.name; });
}

static std::string test_name_list(const std::vector<Compiler::TestCase> &tests)
{
    return comma_list(tests, Compiler::test_display_name);
}

// the declared targets this invocation builds, or a refusal naming what there was to choose from.
//
// **`--target` has no checker on its option row**, deliberately: the names belong to the manifest and not
// to echoc, so this is the first point at which one can be wrong, and it is the only one that can say what
// the right ones were
static bool select_targets(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Parser::ModuleManifest &entry,
    std::vector<Parser::ModuleTarget> &out
)
{
    // **the subcommand decides which kinds it is even looking at**, and without this arm `echoc build`
    // starts building the test targets a manifest declares: they are in the same list, and a target with no
    // entry file would then be handed to codegen as a program whose body is every file root
    const Parser::TargetKind wanted = driver.subcommand == Compiler::Subcommand::t_test
        ? Parser::TargetKind::t_test
        : Parser::TargetKind::t_executable;

    std::vector<Parser::ModuleTarget> candidates;

    for (const Parser::ModuleTarget &target : entry.targets) {
        if (target.kind == wanted) {
            candidates.push_back(target);
        }
    }

    if (driver.targets.empty()) {
        out = candidates;
        return true;
    }

    for (const std::string &named : driver.targets) {
        auto found = std::find_if(candidates.begin(), candidates.end(),
            [&named](const Parser::ModuleTarget &target) { return target.name == named; });

        if (found == candidates.end()) {
            diagnostics.render_untyped("No Such Target", fmt::format(
                "'{}' declares no target called '{}'. It declares: {}.",
                entry.name, named, target_name_list(candidates)));
            return false;
        }

        // a name written twice is one program, not two links over one path
        if (std::none_of(out.begin(), out.end(),
                [&named](const Parser::ModuleTarget &target) { return target.name == named; })) {
            out.push_back(*found);
        }
    }

    return true;
}

// **the modules whose `test` blocks this build compiles**, and the sole answer to that question.
//
// two readers, and they have to agree or the cache is unsound rather than merely ineffective:
// Parser::ModuleParser, which decides what the token filter keeps, and Compiler::compute_module_keys, which
// decides what object those tokens are stored under.
//
// only the roots, never a dependency - a project's `echoc test` runs the project's tests, and a library it
// depends on is somebody else's module with somebody else's tests. That is the separation `Invocation::roots`
// already exists for, applied to a second question
static std::set<std::string> resolve_test_modules(
    const Compiler::DriverOptions &driver,
    const Invocation &invocation
)
{
    std::set<std::string> result;

    if (driver.subcommand != Compiler::Subcommand::t_test) {
        return result;
    }

    for (const Parser::ModuleManifest &manifest : invocation.manifests) {
        if (manifest_is_a_root(manifest, invocation.roots)) {
            result.insert(manifest.name);
        }
    }

    // loose `.eco` files are a module of their own, and the one an invocation pointed at most directly
    if (!invocation.sources.empty()) {
        result.insert(ECO_MAIN_MODULE_NAME);
    }

    return result;
}

// **which programs this invocation produces, and where each one goes.**
//
// the one owner of *"a build that points at several roots and gives no sources has no answer
// to which of these is the program"*. a manifest naming its targets is that answer written
// down, so this is where the two meet
static bool resolve_programs(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    Invocation &out
)
{
    // an entry point has to come from somewhere, and there are two natural spellings because there are two
    // natural shapes of program: a script is a file, an application is a project.
    //
    // loose sources on the command line become the `main` module. Otherwise the program is *the manifest
    // this invocation pointed at* - the one `-m`, or the module.eco in the working directory - whatever it
    // happens to call itself. A build that points at several roots and gives no sources has no answer to
    // "which of these is the program", and guessing would pick one silently
    const Parser::ModuleManifest *entry = nullptr;

    if (out.sources.empty()) {
        if (out.roots.size() != 1) {
            if (out.roots.empty()) {
                std::cerr << "No source files provided, and no 'module.eco' in the working directory."
                          << std::endl;
            }
            else {
                std::cerr << "Several manifests were given and no source files, so it is ambiguous which "
                             "module is the program. Name its sources on the command line, or build one "
                             "manifest at a time." << std::endl;
            }
            return false;
        }

        // the root's own name, resolved through the loaded set rather than from the path - the manifest
        // decides what its module is called. Through manifest_is_a_root and not a comparison of its own,
        // which is what taught it that `-m lib` and `-m lib/module.eco` are one root
        auto found = std::find_if(out.manifests.begin(), out.manifests.end(),
            [&out](const Parser::ModuleManifest &manifest) {
                return manifest_is_a_root(manifest, out.roots);
            });

        if (found == out.manifests.end()) {
            std::cerr << "Internal: the root manifest '" << out.roots.front().string()
                      << "' is not among the resolved modules." << std::endl;
            return false;
        }

        entry = &*found;
    }
    else {
        const bool manifest_is_the_program = std::any_of(
            out.manifests.begin(), out.manifests.end(),
            [](const Parser::ModuleManifest &manifest) { return manifest.name == ECO_MAIN_MODULE_NAME; });

        if (manifest_is_the_program) {
            std::cerr << "A manifest already declares the '" << ECO_MAIN_MODULE_NAME
                      << "' module, so the source files on the command line have nowhere to go."
                      << std::endl;
            return false;
        }
    }

    // **the layout before the programs**, because a target's binary is a path this owns. It is a function
    // of the entry manifest and `--build-dir`, both of which are settled by here, and resolving it once is
    // what stops two answers disagreeing about a directory one of them has written into
    out.layout = Compiler::BuildLayout::resolve(driver.build_dir, entry, fallback_scratch_dir());

    // **the one refusal a build directory can earn, and it is asked before anything is built.** An
    // unwritable store is never fatal - it is an optimization - but a directory that is somebody else's
    // is a build pointed at the wrong place, and writing into it and reporting nothing is how a later
    // `echoc clean` would delete their work
    const std::string foreign = out.layout.first_foreign_directory(out.manifests);

    if (!foreign.empty()) {
        diagnostics.render_untyped("Build Directory In Use", foreign);
        return false;
    }

    // **a test run is one compile, whatever it was pointed at.** There is no entry file - in test mode no
    // file root becomes the program at all - so there is nothing here for a target to vary, and every
    // declared one it selected narrows which tests run instead. That is why this arm is ahead of all of the
    // executable reasoning below rather than a case inside it: none of "which of these is the program",
    // "run runs one" or "-o is one path" is a question a test invocation has
    if (driver.subcommand == Compiler::Subcommand::t_test) {
        // **a declared test target narrows only when it is asked for by name.** `echoc test` runs every test
        // the module has, which is what a person means by it - so an unnamed target is a *saved* selection
        // rather than one in force. Selecting all of them instead would make two targets mean the union of
        // their narrowings, and a module declaring a bare `#[target: test]` beside a narrow one would then
        // run the narrow one's tests and call that everything
        if (entry != nullptr && !driver.targets.empty()
            && !select_targets(driver, diagnostics, *entry, out.test_targets)) {
            return false;
        }

        if (entry == nullptr && !driver.targets.empty()) {
            diagnostics.render_untyped("No Such Target",
                "'--target' names a test target a manifest declares, and this run's tests are those of "
                "the source files on the command line. Narrow them with '--filter' instead.");
            return false;
        }

        Program test_program;
        test_program.entry_module = entry == nullptr ? ECO_MAIN_MODULE_NAME : entry->name;

        // **which scopes a test run opens is not which targets it selected.** `out.test_targets` is a
        // *selection* and stays empty on a bare `echoc test` on purpose, one line above; the sources a
        // test target declares are a different question and every test target of every module this
        // invocation pointed at answers it. A module compiles its tests or it does not - `test_modules`
        // is already that per-module answer, and the two must not drift apart into two readings
        for (const Parser::ModuleManifest &manifest : out.manifests) {
            if (out.test_modules.find(manifest.name) == out.test_modules.end()) {
                continue;
            }

            for (const Parser::ModuleTarget &target : manifest.targets) {
                if (target.kind == Parser::TargetKind::t_test) {
                    test_program.active_targets[manifest.name].insert(target.name);
                }
            }
        }

        out.programs.push_back(std::move(test_program));

        return true;
    }

    // loose sources are the program, so a manifest reached with `-m` is a library to them however many
    // programs it declares for its own sake.
    //
    // **the *executable* targets and not every target**, or a manifest declaring only `#[target: test]`
    // would fall through to the loop below and build no program at all, silently
    if (entry == nullptr || !module_declares_a_program(*entry)) {
        if (!driver.targets.empty()) {
            diagnostics.render_untyped("No Such Target", entry == nullptr
                ? "'--target' names a program a manifest declares, and this build's program is the source "
                  "files on the command line."
                : fmt::format(
                    "'{}' declares no programs, so it produces the one its module is. Add "
                    "'#[target: exe {{ name: ..., entry: ... }}]' to its manifest to give it more.",
                    entry->name));
            return false;
        }

        if (driver.subcommand == Compiler::Subcommand::t_build && driver.output.empty()) {
            // **the refusal `-o` cannot make from argv alone.** this is the function that knows
            // whether a manifest named its own binaries
            diagnostics.render_untyped("No Output File", fmt::format(
                "'build' needs '-o, --output <file>' - nothing here names the binary. {}",
                entry == nullptr
                    ? "A program built from loose source files has no other name to take."
                    : fmt::format("'{}' declares no programs, so its manifest does not name one either.",
                        entry->name)));
            return false;
        }

        out.programs.push_back(Program {
            /*name=*/{},
            entry == nullptr ? ECO_MAIN_MODULE_NAME : entry->name,
            /*entry_file=*/{},
            driver.output
        });

        return true;
    }

    std::vector<Parser::ModuleTarget> selected;

    if (!select_targets(driver, diagnostics, *entry, selected)) {
        return false;
    }

    // **one invocation runs one program**, so `run` is where several is a refusal rather than a loop.
    // Named rather than guessed at: picking the first would run something nobody asked for
    if (driver.subcommand == Compiler::Subcommand::t_run && selected.size() > 1) {
        diagnostics.render_untyped("Which Program", fmt::format(
            "'{}' declares {} programs and 'run' runs one. Name it with '--target': {}.",
            entry->name, selected.size(), target_name_list(selected)));
        return false;
    }

    // `-o` is one path, so it can only mean something when one binary is being written
    if (!driver.output.empty() && selected.size() > 1) {
        diagnostics.render_untyped("Too Many Targets For One Output", fmt::format(
            "'-o' names one file and {} targets are being built. Name the one you meant with "
            "'--target', or drop '-o' and let each go to its own binary under '{}'.",
            selected.size(), out.layout.module_dir(*entry).string()));
        return false;
    }

    for (const Parser::ModuleTarget &target : selected) {
        Program program {
            target.name,
            entry->name,
            target.entry,
            driver.output.empty() ? out.layout.target_binary(*entry, target.name) : driver.output
        };

        // **only the target being built, and only in the module that declared it.** Two targets of one
        // module are two programs here, so each opens its own scope and neither sees the other's - which
        // is what makes their module keys differ and stops the second linking the first's object
        program.active_targets[entry->name].insert(target.name);

        out.programs.push_back(std::move(program));
    }

    return true;
}

// everything settled before the first program is compiled: the platform, the module graph, where artifacts
// go, and which programs there are to build
static bool resolve_invocation(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    Invocation &out
)
{
    if (!resolve_target_facts(driver, diagnostics, out.target_facts)) {
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
                driver.options.target_cpu,
                driver.options.target_features,
                subtarget,
                error)) {
            diagnostics.render_untyped("Invalid Target", error);
            return false;
        }
    }

    {
        Compiler::ScopedPhase phase("resolve manifests");

        // the invocation's facts, not a second resolution: a manifest may gate its `#[sources:]`, so the
        // list of files and the conditions inside those files have to be decided by the same answer
        if (!resolve_manifests(
                driver.modules,
                !driver.no_stdlib,
                driver.sources.empty(),
                diagnostics, out.target_facts, driver.package_dir, out.manifests, out.roots)) {
            return false;
        }
    }

    size_t missing_files = 0;
    out.sources = get_file_list_from_args(driver.sources, missing_files);

    // a named file that is not there fails the build, even when others compiled. it is an input the
    // user asked for, so carrying on would produce a binary missing whatever was in it - reported by
    // name above, so nothing more is owed here
    if (missing_files > 0) {
        return false;
    }

    // after the graph and the sources, both of which it reads, and before anything parses
    out.test_modules = resolve_test_modules(driver, out);

    return resolve_programs(driver, diagnostics, out);
}

// the front end for **one program**: parse the whole bundle, then run the semantic passes over it.
//
// what it is handed rather than deriving is exactly what a second program would derive identically - the
// platform, the module graph, the layout - and what it settles is what differs between two of them, which
// today is nothing but the entry file. That it *could* therefore run once for every target is true and is
// not this: `compile_bundle` builds the LLVMContext its units live in, so proving it re-entrant over one
// AST::Bundle is a separate piece of work with its own way of going quietly wrong
static bool run_front_end(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Invocation &invocation,
    const Program &program,
    AST::Bundle &bundle,
    FrontEnd &out
)
{
    out.invocation = &invocation;
    out.program = &program;
    out.compiled = compiled_manifests(invocation, program);

    // **after the facts and not before**, which is why the parser is built here rather than handed in: it
    // takes them at construction, so there is no window in which one exists that has not been told what
    // platform it is reading for. Neither subcommand touches it once the front end is done
    Parser::ModuleParser parser(out.target_facts(), out.test_modules());

    {
        Compiler::ScopedPhase phase("parse");
        if (build_bundle(driver, diagnostics, invocation, program, out.compiled, bundle, parser) != 0) {
            return false;
        }
    }

    // **settled before anything is parsed.** AST::TypeChecker reads it - it refuses
    // `mem::live_allocations()` when nothing is counting - so it has to exist by the semantic
    // passes, and that is the whole of what it owes
    out.options = driver.options;

    {
        Compiler::ScopedPhase phase("semantic passes");
        if (run_semantic_passes(driver, diagnostics, bundle, out.options) != 0) {
            return false;
        }
    }

    if (needs_cache_keys(driver)
        && !compute_cache_keys(
            driver, diagnostics, out.manifests(), out.options, out.target_facts(), out.test_modules(),
            program.active_targets, out.cache_keys)) {
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
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const FrontEnd &front,
    std::vector<Compiler::LinkRequirement> &out
)
{
    for (auto manifest = front.manifests().rbegin(); manifest != front.manifests().rend(); ++manifest) {
        Compiler::merge_link_requirements(front.contribution(**manifest).link, out);
    }

    const std::vector<std::string> &spelled_on_command_line = driver.link;

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
                spelled, here, front.target_facts(), /*declared_by=*/"", requirement, error)) {
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
// into a loadable library instead and the JIT opens it - see Backend::prepare_execution
//
// exactly one of the two is filled, which is what `for_jit` selects - returned together rather than as two
// out-parameters, so neither caller has to name a variable it is going to throw away
struct CModuleBuilds
{
    std::vector<Compiler::LinkRequirement> objects;
    std::vector<NativeLibrary> libraries;
};

static bool build_c_modules(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const FrontEnd &front,
    bool for_jit,
    CModuleBuilds &out
)
{
    const bool explaining = driver.explains(Compiler::ExplainKind::t_cache);

    const std::filesystem::path scratch_dir = front.layout().scratch_cc_dir();

    std::vector<std::string> explain;

    for (const Parser::ModuleManifest *entry : front.manifests()) {
        const Parser::ModuleManifest &manifest = *entry;
        const Parser::ModuleContribution contribution = front.contribution(manifest);

        if (contribution.cc.empty()) {
            continue;
        }

        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_cc, manifest.name);

        const size_t sources = contribution.cc.sources.size();
        step.summary(fmt::format("{} source{}", sources, sources == 1 ? "" : "s"));

        const std::filesystem::path cache_dir = front.layout().module_cc_dir(manifest);

        Compiler::CBuildResult result;
        std::string error;

        if (!Compiler::build_c_sources(
                contribution.cc, front.options, cache_dir, scratch_dir / manifest.name,
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
        Compiler::partition_link_requirements(contribution.link, own_objects, link_words);

        std::filesystem::path library;

        if (!Compiler::build_c_shared_library(
                contribution.cc, result, link_words, cache_dir, scratch_dir / manifest.name,
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
// **the whole of why this is in the driver rather than in Backend::prepare_execution.** The backend can
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
        // ahead of prepare_execution entirely is the version of that ordering nobody can get wrong
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
// the program's own name covers the last case, a manifest discovered rather than asked for
std::string program_name(const Compiler::DriverOptions &driver, const Program &program)
{
    const std::vector<std::string> &sources = driver.sources;
    if (!sources.empty()) {
        return sources.front();
    }

    // **a declared target is asked before the module root**, because it is the more specific answer and
    // the only one that tells two programs of one module apart
    if (!program.name.empty()) {
        return program.name;
    }

    const std::vector<std::string> &modules = driver.modules;
    if (!modules.empty()) {
        return modules.front();
    }

    return program.entry_module;
}

// the whole-program optimizer, run iff `-O` asked for it - the same answer for both subcommands.
//
// read off the flag rather than off the resolved options. making this unconditional on `build`
// would make `-O` a switch it accepted and silently ignored, and leave no way at all to see
// what codegen emitted for a release build, since `-p` only ever showed the optimizer's output
static void optimize_if_asked(const Compiler::DriverOptions &driver, LLVMCompiler &compiler)
{
    if (!driver.optimize_is_whole_program()) {
        return;
    }

    Compiler::ScopedPhase phase("optimize");
    Compiler::ProgressStep step(
        Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_optimize);

    compiler.optimize();
    step.finish(true);
}

// **everything between a parsed bundle and a JIT that can be asked for an address**, shared by the two
// subcommands that run one: the cache plan, the link requirements, the C modules, the native libraries the
// JIT must have open before it resolves a symbol, then codegen and the whole-program optimizer.
//
// `run` and `test` do the same work here and differ only in what they do with the compiler afterwards - one
// runs the program, the other calls one definition at a time - so this is one function rather than the two
// copies it was, which is what keeps a change to the JIT path from having to be remembered twice.
//
// nullopt is success; anything else is the exit status the subcommand owes its caller
static std::optional<int> prepare_jit(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    FrontEnd &front,
    AST::Bundle &bundle,
    bool test_mode,
    LLVMCompiler &compiler
)
{
    // the JIT is handed one module, so every unit is merged and there are no per-module objects to store or
    // load - the plan is reported as bypassed rather than not reported at all
    report_cache_plan(
        driver, front.manifests(), front.cache_keys, ModulePlan{}, front.entry_module(),
        /*bypassed=*/true);

    // **before codegen**, because a C source that does not compile is a build that is going to fail either
    // way, and finding out after the whole Echo program has been lowered wastes the wait. It also puts the
    // `cc` phase where `-t` reads naturally: beside `parse`, not inside `jit`
    std::vector<Compiler::LinkRequirement> link;
    CModuleBuilds c_builds;

    if (!collect_link_requirements(driver, diagnostics, front, link)) {
        return 1;
    }

    {
        Compiler::ScopedPhase phase("cc");

        if (!build_c_modules(driver, diagnostics, front, /*for_jit=*/true, c_builds)) {
            return 1;
        }
    }

    if (!load_native_libraries(diagnostics, link, c_builds.libraries)) {
        return 1;
    }

    compiler.set_entry(front.entry_module(), front.entry_file());

    // **no file root becomes the program.** Whatever application this module is does not run: a test asked
    // for is a test, not a test after the program it sits beside
    compiler.set_test_mode(test_mode);

    try {
        Compiler::ScopedPhase phase("codegen");

        // **inside the try, deliberately.** Unwinding destroys it before the catch below runs, so the
        // failed row is committed ahead of the diagnostic that explains it - which is the order every
        // other step reaches by calling finish() and this one gets for nothing
        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(), Compiler::ProgressPhase::t_codegen);

        compiler.compile_bundle(bundle);

        // the JIT can only be handed one module, so both of these paths always merge
        compiler.link_into_main();
        step.finish(true);
    } catch (Compiler::ASTCompilerException &e) {
        return report_compiler_exception(diagnostics, e);
    }

    optimize_if_asked(driver, compiler);

    if (driver.prints(Compiler::PrintKind::t_ir)) {
        compiler.printIR(false);
    }

    return std::nullopt;
}

int main_run(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
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
    if (driver.options.emitting_debug_info()) {
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
    Invocation invocation;
    if (!resolve_invocation(driver, diagnostics, invocation)) {
        return 1;
    }

    // resolve_programs already refused several, so this is the one it settled on
    const Program &program = invocation.programs.front();

    FrontEnd front;
    if (!run_front_end(driver, diagnostics, invocation, program, bundle, front)) {
        return 1;
    }

    const std::string &entry_module = front.entry_module();

    LLVMCompiler compiler(front.options);

    if (const std::optional<int> failed = prepare_jit(
            driver, diagnostics, front, bundle, /*test_mode=*/false, compiler)) {
        return failed.value();
    }

    // `argv[0]` is the program's own name, and under `run` the honest answer is the source file the
    // entry module was read from - not `echoc`, which is the process but not the program. So the tail
    // the driver split off a `--` is prepended with it rather than used as-is
    std::vector<std::string> argv = { program_name(driver, program) };
    argv.insert(argv.end(), driver.program_arguments.begin(), driver.program_arguments.end());

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
        status = compiler.prepare_execution() ? compiler.run_main(argv, environment) : 1;
    }

    // after the program, because the prune happened inside the run - the same position `[timings]` takes,
    // and for the same reason
    if (driver.explains(Compiler::ExplainKind::t_prune)) {
        std::cout << compiler.prune_report();
    }

    std::cout << Compiler::PhaseTimings::instance().report();

    // after the program, because a C module's loadable library lives there and was open for the whole of it
    front.layout().discard_temporary_scratch();

    // the program's own status, so `echoc run` exits the way it did. Today that is always 0 on this
    // path - the entry point's epilogue returns 0 and every other ending goes through libc's `exit`
    // from inside the JIT, which never comes back here at all
    return status;
}

// **every test the bundle declared, in the order the modules were parsed.**
//
// only the modules whose tests were compiled have any, so this needs no filter of its own: the token filter
// already dropped every test block of every module the invocation did not point at, which is what makes
// `Module::tests` empty for the rest.
//
// the symbol is mangled here rather than carried on the record, because AST::mangle_function_name is the one
// thing that knows how - and a symbol stored at parse time would be stored before the mangler had run
static std::vector<Compiler::TestCase> collect_tests(const AST::Bundle &bundle)
{
    std::vector<Compiler::TestCase> tests;

    for (const auto &module_ptr : bundle.modules) {
        for (const AST::TestDeclaration &declared : module_ptr->tests) {
            if (declared.decl == nullptr) {
                continue;
            }

            const AST::File *file = declared.decl->declared_in.file;

            tests.push_back(Compiler::TestCase {
                module_ptr->name,
                file != nullptr ? file->get_path() : std::filesystem::path{},
                declared.group,
                declared.name,
                AST::mangle_function_name(declared.decl)
            });
        }
    }

    return tests;
}

// what this invocation asked to run, from the command line and from any `#[target: test]` it selected.
//
// **both spellings build one selection**, which is what keeps a declared target a saved filter rather than a
// second engine: `--filter group:x` and `#[target: test { groups: ["x"] }]` become the same TestFilter, and a
// run naming both gets the union - the same way two `--filter` words do
static bool resolve_test_selection(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Invocation &invocation,
    Compiler::TestSelection &out
)
{
    for (const std::string &spelled : driver.filters) {
        Compiler::TestFilter filter;
        std::string error;

        if (!Compiler::parse_test_filter(spelled, filter, error)) {
            diagnostics.render_untyped("Invalid Test Filter", error);
            return false;
        }

        out.filters.push_back(filter);
    }

    for (const Parser::ModuleTarget &target : invocation.test_targets) {
        out.add_declared(target.groups, target.files);
    }

    return true;
}

// **compiles the module and runs its tests, each in a process of its own.**
//
// down to the codegen step this is main_run, and it has to be: a test run is the JIT, so it needs the same
// link requirements resolved, the same C modules built and the same native libraries open - which is why
// both go through prepare_jit rather than each spelling it. What differs begins after the compile: there is
// no program to run, only a list of definitions to call one at a time
int main_test(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Compiler::TerminalCapabilities &capabilities,
    const char *const *environment)
{
    // **refused here rather than at the first test**, which is the call `#[link: framework]` makes about
    // Darwin: a platform that cannot do the thing is told so where the thing was asked for, and every test
    // "failing" for the same reason is not a report anybody can act on
    if (!Compiler::test_isolation_available()) {
        diagnostics.render_untyped("Tests Cannot Be Isolated",
            "'echoc test' runs each test in a process of its own, and this platform has no fork. Every "
            "other subcommand is unaffected.");
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();

    auto bundle = AST::Bundle();

    Invocation invocation;
    if (!resolve_invocation(driver, diagnostics, invocation)) {
        return 1;
    }

    // resolve_programs pushes exactly one for a test run: a test run is one compile whatever it was
    // pointed at, the targets it selected being a selection rather than a program each
    const Program &program = invocation.programs.front();

    FrontEnd front;
    if (!run_front_end(driver, diagnostics, invocation, program, bundle, front)) {
        return 1;
    }

    Compiler::TestSelection selection;
    if (!resolve_test_selection(driver, diagnostics, invocation, selection)) {
        return 1;
    }

    const std::vector<Compiler::TestCase> declared = collect_tests(bundle);
    const std::vector<Compiler::TestCase> selected = Compiler::select_tests(declared, selection);

    // **a filter that matched nothing is a refusal, and a module with no tests is not.** The two are
    // different news: one is a mistake in what was typed, where naming what there was to choose from is the
    // whole of the help - and the other is a project that has not written a test yet, which is a fact rather
    // than an error. A suite reporting success having run nothing is the failure mode this exists to prevent
    if (selected.empty()) {
        if (declared.empty()) {
            Compiler::ProgressReporter::instance().close(
                fmt::format("'{}' declares no tests", front.entry_module()),
                Compiler::progress_elapsed_ms(started));

            std::cout << Compiler::PhaseTimings::instance().report();
            front.layout().discard_temporary_scratch();

            return 0;
        }

        diagnostics.render_untyped("No Tests Selected", fmt::format(
            "nothing this run compiled matches those filters. It declares {} test{}: {}.",
            declared.size(), declared.size() == 1 ? "" : "s", test_name_list(declared)));
        return 1;
    }

    LLVMCompiler compiler(front.options);

    if (const std::optional<int> failed = prepare_jit(
            driver, diagnostics, front, bundle, /*test_mode=*/true, compiler)) {
        return failed.value();
    }

    // **the selected tests are the prune's roots**, beside `main`. Without this the JIT deletes every one of
    // them - nothing reaches a test from the entry point, that being the whole of what a test is - and
    // function_address answers 0 for each. It also means `--filter` narrows the machine code and not only
    // the run
    std::vector<std::string> roots;
    for (const Compiler::TestCase &test : selected) {
        roots.push_back(test.symbol);
    }

    compiler.set_jit_roots(std::move(roots));

    if (!compiler.prepare_execution()) {
        return 1;
    }

    // the prologue, once, so `std::env::args()` inside a test reads the process the tests were started from.
    // `main` in test mode is that capture and a `ret 0`, so this runs no statement anybody wrote
    {
        Compiler::ScopedPhase phase("jit");

        std::vector<std::string> argv = { program_name(driver, program) };

        if (compiler.run_main(argv, environment) != 0) {
            diagnostics.render_untyped("Test Prologue Failed",
                "the compiled entry point did not return 0, so the tests were not started.");
            return 1;
        }
    }

    // **the facts main() settled, never a second resolve.** Two places deciding independently whether to
    // emit an escape sequence is how a redirected stream ends up with half of them in it - and these are
    // the same facts the diagnostics and the checklist were built from
    Compiler::TestReporter reporter(
        std::cout,
        Compiler::ProgressReporter::instance(),
        capabilities,
        driver.verbose ? Compiler::TestDetail::t_listing : Compiler::TestDetail::t_counter);

    reporter.begin(selected.size());

    for (const Compiler::TestCase &test : selected) {
        const uint64_t address = compiler.function_address(test.symbol);

        // **loud rather than a call through null.** A test the prune dropped is a mistake in the root set
        // above, and there is no version of it that reads as a test failing
        if (address == 0) {
            diagnostics.render_untyped("Test Not Emitted", fmt::format(
                "internal: '{}' has no compiled body under '{}', so it cannot be run.",
                Compiler::test_display_name(test), test.symbol));
            return 1;
        }

        reporter.result(Compiler::run_test_isolated(test, [address]() {
            reinterpret_cast<void (*)()>(address)();
        }));
    }

    const bool passed = reporter.finish();

    if (driver.explains(Compiler::ExplainKind::t_prune)) {
        std::cout << compiler.prune_report();
    }

    std::cout << Compiler::PhaseTimings::instance().report();

    front.layout().discard_temporary_scratch();

    return passed ? 0 : 1;
}

// compiles and links **one** program: its own bundle, its own entry point, its own binary.
//
// a fresh AST::Bundle and a fresh LLVMCompiler per program, so nothing about building two of them is a
// question about re-entrancy - the whole-program merge, the one `@main` symbol and the single LLVMContext
// each stay exactly as true of one program as they ever were. What that costs is the shared code being
// parsed and lowered once per target; what it buys is that none of the invariants below had to move
static int build_one_program(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Invocation &invocation,
    const Program &program
)
{
    auto bundle = AST::Bundle();

    const bool whole_program = driver.whole_program;

    FrontEnd front;
    if (!run_front_end(driver, diagnostics, invocation, program, bundle, front)) {
        return 1;
    }

    const Compiler::CompilerOptions options = front.options;
    const std::string &entry_module = front.entry_module();

    // an optimized or dumped build reuses nothing and stores nothing - see wants_whole_program_module
    const ModulePlan plan = whole_program
        ? ModulePlan{}
        : plan_module_artifacts(front.layout(), front.manifests(), front.cache_keys, entry_module);

    report_cache_plan(driver, front.manifests(), front.cache_keys, plan, entry_module, whole_program);

    // **a reused module gets a row and a rebuilt one does not.** Work that did not happen is the
    // surprising half; which input changed for the ones that did is `--explain-cache`'s question, and
    // answering it twice would put explain_miss's reasoning in two places
    for (const Parser::ModuleManifest *manifest_ptr : front.manifests()) {
        const Parser::ModuleManifest &manifest = *manifest_ptr;

        if (plan.cached.count(manifest.name) > 0) {
            Compiler::ProgressReporter::instance().row(
                Compiler::ProgressPhase::t_cached, manifest.name, "reused",
                Compiler::ProgressState::t_skipped);
        }
    }

    const std::string output = program.output.string();

    // before codegen, for the reason `run` states: a C source that does not compile fails this build
    // whatever the Echo half does, and the wait is the same either way
    std::vector<Compiler::LinkRequirement> link;

    if (!collect_link_requirements(driver, diagnostics, front, link)) {
        return 1;
    }

    {
        Compiler::ScopedPhase phase("cc");

        CModuleBuilds c_builds;

        if (!build_c_modules(
                driver, diagnostics, front, /*for_jit=*/false, c_builds)) {
            return 1;
        }

        // ahead of every library, which partition_link_requirements is what guarantees
        Compiler::merge_link_requirements(c_builds.objects, link);
    }

    LLVMCompiler compiler(options);
    compiler.set_entry(entry_module, front.entry_file());

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

    optimize_if_asked(driver, compiler);

    if (driver.prints(Compiler::PrintKind::t_ir)) {
        compiler.printIR(false);
    }

    // after the merge decision above and before the emit below, so what it prints is what gets written.
    // it optimizes the units it prints unless told not to - the same call emit_objects makes, so the dump
    // and the object cannot disagree
    if (driver.prints(Compiler::PrintKind::t_ir_units)) {
        compiler.print_unit_ir();
    }

    // **unlike a store, this one is not optional.** A cache that cannot be written costs a rebuild; a
    // scratch directory that cannot be written is an object with nowhere to go, so it is worth a sentence
    // here rather than an ofstream failure from inside the backend
    if (front.layout().prepare_scratch_dir() != Compiler::BuildDirState::t_ready) {
        diagnostics.render_untyped("Cannot Build Here", fmt::format(
            "'{}' could not be created, and the objects on the way to '{}' have nowhere to go.",
            front.layout().scratch_dir().string(), output));
        return 1;
    }

    {
        Compiler::ScopedPhase phase("emit + link");
        Compiler::ProgressStep step(
            Compiler::ProgressReporter::instance(),
            Compiler::ProgressPhase::t_emit,
            std::filesystem::path(output).filename().string());

        // **one emit-and-link path, whichever build this is.** a second spelling - an emit_object
        // plus a link_executable over the one unit `emit_objects` skips its way down to anyway,
        // `link_into_main()` having consumed all the others - would drift from this one. what
        // `--optimize whole` depends on is that merge, which happened above and is not in question;
        // with an empty plan `object_for` answers the scratch path, which is the object that arm
        // was handed.
        // The tool that failed has already said what it could, and what it could not say is which
        // manifest asked for each requirement
        const bool linked = emit_and_link_modules(compiler, front.layout(), output, plan, link);

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
    front.layout().discard_temporary_scratch();

    return 0;
}

int main_build(const Compiler::DriverOptions &driver, const AST::DiagnosticRenderer &diagnostics)
{
    // see main_run: the checklist's own clock, and the only one on this path when `-t` is off
    const auto started = std::chrono::steady_clock::now();

    Invocation invocation;
    if (!resolve_invocation(driver, diagnostics, invocation)) {
        return 1;
    }

    const bool several = invocation.programs.size() > 1;

    for (const Program &program : invocation.programs) {
        // **only when there is more than one**, which no invocation that predates targets can be: the
        // e2e corpus byte-compares merged stdout and stderr for six hundred cases, so a line printed
        // unconditionally here would rewrite essentially all of them. It is what makes the `-p` dumps
        // below attributable when a build produces several programs
        if (several) {
            Compiler::ProgressReporter::instance().suspend();
            std::cout << "[target " << program.name << "]" << std::endl;
        }

        if (build_one_program(driver, diagnostics, invocation, program) != 0) {
            // **the first failure stops the build.** run_semantic_passes renders one summary per program
            // and `--diagnostics json` promises diagnostics then *one* of them - and an error in code the
            // targets share would otherwise be reported once per target, which is the same mistake said
            // several times rather than more help
            return 1;
        }
    }

    Compiler::ProgressReporter::instance().close(
        several
            ? fmt::format("built {} targets", invocation.programs.size())
            : fmt::format("built '{}'",
                std::filesystem::path(invocation.programs.front().output).filename().string()),
        Compiler::progress_elapsed_ms(started));

    // once for the invocation rather than once per program: the tree is cumulative, so what it reports is
    // where the whole compile went - which is the question somebody timing a multi-target build has
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
int main_clean(const Compiler::DriverOptions &driver, const AST::DiagnosticRenderer &diagnostics)
{
    Compiler::TargetFacts facts;

    if (!resolve_target_facts(driver, diagnostics, facts)) {
        return 1;
    }

    std::vector<Parser::ModuleManifest> manifests;
    std::vector<std::filesystem::path> roots;

    // the standard library is resolved whether or not it is being removed: `--with-stdlib` needs its directory
    // to name it, and leaving it out would make the line saying it was kept impossible to write
    if (!resolve_manifests(
            driver.modules,
            /*with_stdlib=*/true,
            /*allow_project_discovery=*/true,
            diagnostics, facts, driver.package_dir, manifests, roots)) {
        return 1;
    }

    const Compiler::BuildLayout layout = Compiler::BuildLayout::resolve(
        driver.build_dir, nullptr, {});

    const bool including_stdlib = driver.with_stdlib;
    const bool dry_run = driver.dry_run;

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
            std::cout << "kept (pass --with-stdlib to remove it)" << std::endl;
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
// `--print manifest` is an answer: read the named manifests and stop, without resolving the
// graph. epm uses this to parse a module.eco whose packages are not on disk yet
static int main_print_manifest(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics
)
{
    Compiler::TargetFacts facts;

    if (!resolve_target_facts(driver, diagnostics, facts)) {
        return 1;
    }

    std::vector<std::filesystem::path> named;

    for (const std::string &module : driver.modules) {
        named.push_back(module);
    }

    if (named.empty()) {
        if (const std::optional<std::filesystem::path> discovered = discover_project_manifest()) {
            named.push_back(discovered.value());
        }
    }

    if (named.empty()) {
        diagnostics.render_untyped(
            "No Manifest",
            "'--print manifest' needs a '-m' or a module.eco in the working directory.");
        return 1;
    }

    Parser::ManifestScratch scratch(facts);
    std::optional<std::filesystem::path> missing;
    const std::optional<std::string> json = Parser::written_manifests_json(named, scratch, missing);

    if (!json.has_value()) {
        if (missing.has_value()) {
            diagnostics.render_untyped(
                "No Such Manifest",
                fmt::format("{}: no such manifest - expected a manifest file or a directory holding "
                    "a 'module.eco'.", missing->string()));
        }
        else {
            scratch.bundle.collector.print_issues(diagnostics);
        }

        return 1;
    }

    std::cout << json.value();
    return 0;
}

int main(int argc, char *argv[], char *envp[])
{
    // **parse, then answer, then resolve.** Compiler::parse_command_line owns every rule about what the
    // words mean, including the bare `--` split, so nothing here reaches into argv - and --help and
    // --version come back as answers rather than as an exit taken inside a library, which is what lets
    // the driver choose the stream each of them goes to
    Compiler::CommandLine cli;
    std::string cli_error;

    const bool parsed = Compiler::parse_command_line(argc, argv, cli, cli_error);

    // **the page is drawn with stdout's facts and the refusal with stderr's**, which is the one place in
    // this compiler those legitimately differ: a help page is an answer to a question and belongs on the
    // stream the asker redirected, where everything that reports belongs on the other
    const Compiler::TerminalCapabilities answering = Compiler::TerminalCapabilities::resolve(
        cli.color_choice(), cli.diagnostic_format(), Compiler::TerminalStream::t_stdout);

    const Compiler::CommandLineHelp help(std::cout, answering);

    if (parsed && cli.wants_version) {
        help.render_version();
        return 0;
    }

    if (parsed && cli.wants_help) {
        if (cli.help_topic != nullptr) {
            help.render_option_help(cli.subcommand, *cli.help_topic);
        }
        else {
            help.render_help(cli.subcommand);
        }

        return 0;
    }

    if (!parsed) {
        const Compiler::TerminalCapabilities reporting = Compiler::TerminalCapabilities::resolve(
            cli.color_choice(), cli.diagnostic_format(), Compiler::TerminalStream::t_stderr);

        Compiler::CommandLineHelp(std::cerr, reporting).render_error(cli_error, cli.subcommand);

        return 1;
    }

    // what the invocation *means*, with every implication applied once - see Compiler::DriverOptions.
    // It can still refuse, on a value whose acceptance another owner holds
    Compiler::DriverOptions driver;

    if (!Compiler::resolve_driver_options(cli, driver, cli_error)) {
        std::cerr << cli_error << std::endl;
        return 1;
    }

    // enabled here rather than inside each entry point, so the very first phase a subcommand enters is
    // already being timed
    if (driver.explains(Compiler::ExplainKind::t_time)) {
        Compiler::PhaseTimings::instance().enable();
    }

    // **one renderer, built here and passed down.** Everything that reports takes a reference to it, so
    // there is exactly one answer to what a diagnostic looks like - which is the whole point: this
    // replaced three printers that had each grown their own idea of it
    const Compiler::TerminalCapabilities capabilities
        = Compiler::TerminalCapabilities::resolve(driver.color, driver.format);

    // **stderr, whatever the format.** stdout under `run` belongs to the program being executed, so a
    // compiler writing into it is a compiler whose output cannot be piped - and an editor reading the
    // json form wants one stream that carries diagnostics and nothing else
    const AST::DiagnosticRenderer diagnostics(std::cerr, driver.format, capabilities);

    // **the same stream, and the gate that keeps them from fighting over it.** The checklist is drawn only
    // when stderr is a terminal that can be redrawn, `--silent` was not given, and the format is not the
    // machine-readable one - so a pipe, a CI log and the e2e corpus write nothing and need no
    // configuration, which is the rule `--diagnostics=auto` already follows for the same reason.
    //
    // enabled after the capabilities and not beside PhaseTimings above, because it needs them - and after
    // the renderer, because a row must never be the first thing on a stream a diagnostic is about to fail
    // onto
    if (capabilities.interactive && !driver.silent && !diagnostics.is_machine_readable()) {
        Compiler::ProgressReporter::instance().enable(std::cerr, capabilities);
    }

    if (driver.prints(Compiler::PrintKind::t_manifest)) {
        return main_print_manifest(driver, diagnostics);
    }

    switch (driver.subcommand) {
    case Compiler::Subcommand::t_run:
        return main_run(driver, diagnostics, envp);

    case Compiler::Subcommand::t_clean:
        return main_clean(driver, diagnostics);

    case Compiler::Subcommand::t_build:
        return main_build(driver, diagnostics);

    case Compiler::Subcommand::t_test:
        return main_test(driver, diagnostics, capabilities, envp);

    // unreachable: the parser refuses an invocation with no command, so this is here to make the switch
    // total rather than to be taken
    case Compiler::Subcommand::t_none:
        break;
    }

    return 1;
}
