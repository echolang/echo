#include <iostream>
#include <fstream>
#include <sstream>

#include <argparse.h>
#include <glob.hpp>

#include "eco.h"
#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTMonomorphizer.h"
#include "AST/ASTPointerAdjuster.h"
#include "AST/ASTTypeChecker.h"
#include "Parser/ModuleParser.h"
#include "Compiler/CompilerException.h"
#include "Compiler/LLVM/LLVMCompiler.h"

#if ECO_USE_EMBEDDED_STDLIB
#include "stdlib_embedded.h"
#endif

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


std::vector<std::filesystem::path> get_file_list_from_args(argparse::ArgumentParser &cli, const std::string &arg)
{
    auto path_strings = cli.get<std::vector<std::string>>(arg);

    std::vector<std::filesystem::path> files;

    for (const auto &path_string : path_strings) {
        std::filesystem::path path{path_string};

        // check for wildcards
        if (path_string.find('*') != std::string::npos) {
            auto paths = glob::glob(path_string);

            for (const auto &p : paths) {
                if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
                    files.push_back(p);
                }
            }
        } else {
            if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
                files.push_back(path);
            }
        }
    }

    return files;
}

void print_critical_error(std::string title, std::string message)
{
    std::cout << SH_COLOR_BOLD(SH_COLOR_FRED( << title <<) ) << std::endl;

    for (size_t i = 0; i < title.size(); i++) {
        std::cout << "-";
    }

    std::cout << std::endl;
    std::cout << message << std::endl;
}

int handle_parse(Parser::ModuleParser &parser, Parser::ModuleParser::InputPayload &input)
{
#if ECO_DONT_CATCH_EXCEPTIONS
    parser.parse_input(input);
#else
    try {
        parser.parse_input(input);
    }
    catch (Parser::ModuleParser::TokenizationException &e) {
        print_critical_error("Tokenization Failed", e.what());
        return 1;
    }
#endif

    return 0;
}

// every .eco file the standard library is made of, sorted so the ordering is reproducible
//
// globbed rather than listed, so adding a stdlib file does not need a C++ edit - the previous
// hardcoded list had already fallen behind what is on disk
//
// two directories are skipped. `build/` holds the generated embedded header rather than source
// `sketches/` holds Echo that describes a type the language cannot express yet (string, List) and
// deliberately does not compile - it is design, kept in source form, and the directory name is
// what says so
static std::vector<std::filesystem::path> stdlib_source_files()
{
    std::vector<std::filesystem::path> files;

    const std::filesystem::path root{STDLIB_SOURCE_DIR};
    if (!std::filesystem::exists(root)) {
        return files;
    }

    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".eco") {
            continue;
        }

        const std::string path_string = entry.path().string();
        if (path_string.find("/build/") != std::string::npos
            || path_string.find("/sketches/") != std::string::npos) {
            continue;
        }

        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());

    return files;
}

// builds the bundle both `run` and `build` compile: the stdlib module, then the main module with
// the user's sources. one function rather than two copies, because the copies had already drifted
// - `build` never created a stdlib module at all, so any program calling `mem::` or `math::`
// compiled under `run` and failed under `build`
static int build_bundle(argparse::ArgumentParser &cli, AST::Bundle &bundle, Parser::ModuleParser &parser)
{
    // leaving the standard library out is leaving one module out, nothing more - nothing
    // downstream looks a module up by that name, codegen only ever asks for ECO_MAIN_MODULE_NAME,
    // and the core types are bound by whichever source declares `#[core: ...]` rather than by the
    // stdlib. what the program gives up is `die`, `assert` and the `mem::`/`math::` namespaces,
    // which is the point: a test reading the emitted IR or an AST dump does not want several
    // hundred lines of library standing between its first assertion and the code it is about
    if (!cli.get<bool>("--no-stdlib")) {
        AST::module_handle_t stdlib_handle = bundle.modules.add_module("stdlib");
        auto &stdlib = bundle.modules.get_module(stdlib_handle);

#if ECO_USE_EMBEDDED_STDLIB
        EmbeddedModule::load_stdlib_module(bundle, stdlib);
        parser.parse_module(stdlib, bundle.collector);
#else
        auto stdlib_input = Parser::ModuleParser::InputPayload {
            .files = {},
            .module = stdlib,
            .collector = bundle.collector
        };

        for (const auto &stdlib_file : stdlib_source_files()) {
            stdlib_input.files.push_back(Parser::ModuleParser::InputFile(stdlib_file));
        }

        if (handle_parse(parser, stdlib_input)) {
            throw std::runtime_error("Failed to parse the echo standard library.");
        }
#endif

        // regenerating the embeddable header is a build step, not a compile step. it used to run on
        // every single `echoc run`, which rewrote a tracked file as a side effect of compiling
        if (cli.is_used("--emit-stdlib-header")) {
            AST::write_embedded_module(stdlib, STDLIB_SOURCE_DIR "/build/stdlib_embedded.h");
        }
    }

    AST::module_handle_t module_handle = bundle.modules.add_module(ECO_MAIN_MODULE_NAME);
    auto &module = bundle.modules.get_module(module_handle);

    auto input = Parser::ModuleParser::InputPayload {
        .files = {},
        .module = module,
        .collector = bundle.collector
    };

    auto source_files = get_file_list_from_args(cli, "source");
    if (source_files.empty()) {
        std::cerr << "No source files provided." << std::endl;
        return 1;
    }

    for (const auto &source_file : source_files) {
        input.files.push_back(Parser::ModuleParser::InputFile(source_file));
    }

    if (handle_parse(parser, input)) {
        return 1;
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
static int run_semantic_passes(argparse::ArgumentParser &cli, AST::Bundle &bundle)
{
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

    AST::TypeChecker(bundle).run();

    print_resolved_ast(cli, bundle);

    bundle.collector.print_issues();
    if (bundle.collector.has_critical_issues()) {
        std::cout << "Critical issues found, cannot compile." << std::endl;
        return 1;
    }

    return 0;
}

// resolves --debug/--release against the subcommand's default. one function because the two
// subcommands disagree only about the default, and a second spelling of the rule would let them
// drift - `build` is a release build unless told otherwise, matching that it already always optimizes
//
// cannot fail: the mutually exclusive group refuses both flags at parse time
static Compiler::CompilerOptions resolve_options(
    argparse::ArgumentParser &cli, Compiler::BuildMode fallback)
{
    if (cli.get<bool>("--debug")) {
        return { Compiler::BuildMode::t_debug };
    }

    if (cli.get<bool>("--release")) {
        return { Compiler::BuildMode::t_release };
    }

    return { fallback };
}

int main_run(argparse::ArgumentParser &cli)
{
    auto bundle = AST::Bundle();
    auto parser = Parser::ModuleParser();

    if (build_bundle(cli, bundle, parser) != 0) {
        return 1;
    }

    if (run_semantic_passes(cli, bundle) != 0) {
        return 1;
    }

    // compile the module
    LLVMCompiler compiler(resolve_options(cli, Compiler::BuildMode::t_debug));

    try {
        compiler.compile_bundle(bundle);
    } catch (Compiler::ASTCompilerException &e) {
        auto issue = &e.issue();
        std::cout << "Compiler Exception: " << e.what() << std::endl;
        std::cout << "Issue at " << issue->code_ref.token_slice.startt().line << ":" << issue->code_ref.token_slice.startt().char_offset << std::endl;
        std::cout << issue->message() << std::endl;
        std::cout << issue->code_ref.get_referenced_code_excerpt() << std::endl;

        // a module whose codegen threw part way through a function is not runnable. printing the
        // diagnostic and then JITing it anyway is how this path used to report success on a failed
        // compile - the exit code said 0 and MCJIT ran over a half-built module
        return 1;
    }

    if (cli.get<bool>("--optimize")) {
        compiler.optimize();
    }

    if (cli.get<bool>("--print-ir")) {
        compiler.printIR(false);
    }

    compiler.run_code();

    return 0;
}

int main_build(argparse::ArgumentParser &cli)
{
    auto bundle = AST::Bundle();
    auto parser = Parser::ModuleParser();

    if (build_bundle(cli, bundle, parser) != 0) {
        return 1;
    }

    if (run_semantic_passes(cli, bundle) != 0) {
        return 1;
    }

    // compile the module
    LLVMCompiler compiler(resolve_options(cli, Compiler::BuildMode::t_release));

    try {
        compiler.compile_bundle(bundle);
        compiler.optimize();
    } catch (Compiler::ASTCompilerException &e) {
        auto issue = &e.issue();
        std::cout << "Compiler Exception: " << e.what() << std::endl;
        std::cout << "Issue at " << issue->code_ref.token_slice.startt().line << ":" << issue->code_ref.token_slice.startt().char_offset << std::endl;
        std::cout << issue->message() << std::endl;
        std::cout << issue->code_ref.get_referenced_code_excerpt() << std::endl;

        // same as main_run, and worse: the next thing here is make_exec, which would run an
        // object-emission pass over a module that may be null or only partly linked
        return 1;
    }

    if (cli.get<bool>("--print-ir")) {
        compiler.printIR(false);
    }

    // ensure the output file is set
    if (!cli.present("-o")) {
        std::cerr << "No output file specified." << std::endl;
        return 1;
    }

    if (!compiler.make_exec(cli.get<std::string>("-o"))) {
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[]) 
{
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
            .help("Compile without the standard library. 'die', 'assert' and 'mem::' are then undeclared.")
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
        cli.parse_args(argc, argv);
    }
    catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << cli;
        return 1;
    }

    if (cli.is_subcommand_used(run_command)) {
        return main_run(run_command);
    }
    else if (cli.is_subcommand_used(build_command)) {
        return main_build(build_command);
    }
    else {
        std::cerr << cli;
        return 1;
    }
}