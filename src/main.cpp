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

    for (const auto& path_string : path_strings) {
        std::filesystem::path path{path_string};

        // check for wildcards
        if (path_string.find('*') != std::string::npos) {
            auto paths = glob::glob(path_string);

            for (const auto& p : paths) {
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

void print_critical_error(std::string title, std::string message) {
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

int main_run(argparse::ArgumentParser &cli)
{
    auto bundle = AST::Bundle();
    auto parser = Parser::ModuleParser();

    AST::module_handle_t stdlib_handle = bundle.modules.add_module("stdlib");
    auto &stdlib = bundle.modules.get_module(stdlib_handle);

#if ECO_USE_EMBEDDED_STDLIB
    EmbeddedModule::load_stdlib_module(bundle, stdlib);
    parser.parse_module(stdlib, bundle.collector);
#else
    auto stdlib_input = Parser::ModuleParser::InputPayload {
        .files = {
            // symbolic
            Parser::ModuleParser::InputFile(STDLIB_SOURCE_DIR "/symbolic/math.eco"),

            // math
            Parser::ModuleParser::InputFile(STDLIB_SOURCE_DIR "/math/functions.eco")
        },
        .module = stdlib,
        .collector = bundle.collector
    };

    if (handle_parse(parser, stdlib_input)) {
        throw std::runtime_error("Failed to parse the echo standard library.");
    }

    // dump the stdlib module into an embedabble cpp file
    AST::write_embedded_module(stdlib, STDLIB_SOURCE_DIR "/build/stdlib_embedded.h");
#endif

    AST::module_handle_t module_handle = bundle.modules.add_module("main");
    auto &module = bundle.modules.get_module(module_handle);

    auto input = Parser::ModuleParser::InputPayload {
        .files = {},
        .module = module,
        .collector = bundle.collector
    };

    // attach the files to the input
    auto source_files = get_file_list_from_args(cli, "source");
    if (source_files.empty()) {
        std::cerr << "No source files provided." << std::endl;
        return 1;
    }

    for (const auto& source_file : source_files) {
        input.files.push_back(Parser::ModuleParser::InputFile(source_file));
    }

    if (handle_parse(parser, input)) {
        return 1;
    }

    if (cli.get<bool>("--print-ast")) {
        for (const auto& mod : bundle.modules) {
            std::cout << "Module: " << mod->debug_description() << std::endl;
        }
    }

    bundle.collector.print_issues();
    if (bundle.collector.has_critical_issues()) {
        std::cout << "Critical issues found, cannot compile." << std::endl;
        return 1;
    }

    // compile the module
    LLVMCompiler compiler;

    try {
        compiler.compile_bundle(bundle);
    } catch (Compiler::ASTCompilerException &e) {
        auto issue = &e.issue();
        std::cout << "Compiler Exception: " << e.what() << std::endl;
        std::cout << "Issue at " << issue->code_ref.token_slice.startt().line << ":" << issue->code_ref.token_slice.startt().char_offset << std::endl;
        std::cout << issue->message() << std::endl;
        std::cout << issue->code_ref.get_referenced_code_excerpt() << std::endl;
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

    AST::module_handle_t module_handle = bundle.modules.add_module("main");
    auto &module = bundle.modules.get_module(module_handle);

    auto parser = Parser::ModuleParser();
    auto input = Parser::ModuleParser::InputPayload {
        .files = {},
        .module = module,
        .collector = bundle.collector
    };

    // attach the files to the input
    auto source_files = get_file_list_from_args(cli, "source");
    if (source_files.empty()) {
        std::cerr << "No source files provided." << std::endl;
        return 1;
    }

    for (const auto& source_file : source_files) {
        input.files.push_back(Parser::ModuleParser::InputFile(source_file));
    }

    if (handle_parse(parser, input)) {
        return 1;
    }

    if (cli.get<bool>("--print-ast")) {
        std::cout << "Module: " << module.debug_description() << std::endl;
    }

    bundle.collector.print_issues();
    if (bundle.collector.has_critical_issues()) {
        std::cout << "Critical issues found, cannot compile." << std::endl;
        return 1;
    }

    // compile the module
    LLVMCompiler compiler;

    try {
        compiler.compile_bundle(bundle);
        compiler.optimize();
    } catch (Compiler::ASTCompilerException &e) {
        auto issue = &e.issue();
        std::cout << "Compiler Exception: " << e.what() << std::endl;
        std::cout << "Issue at " << issue->code_ref.token_slice.startt().line << ":" << issue->code_ref.token_slice.startt().char_offset << std::endl;
        std::cout << issue->message() << std::endl;
        std::cout << issue->code_ref.get_referenced_code_excerpt() << std::endl;
    }

    if (cli.get<bool>("--print-ir")) {
        compiler.printIR(false);
    }

    // ensure the output file is set
    if (!cli.present("-o")) {
        std::cerr << "No output file specified." << std::endl;
        return 1;
    }

    compiler.make_exec(cli.get<std::string>("-o"));

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
    for (auto& command : {std::ref(run_command), std::ref(build_command)}) {
        command.get().add_argument("-p", "--print-ir")
            .help("Print the LLVM IR to the console.")
            .default_value(false)
            .implicit_value(true);

        command.get().add_argument("-a", "--print-ast")
            .help("Print the AST to the console.")
            .default_value(false)
            .implicit_value(true);
        
        command.get().add_argument("-O", "--optimize")
            .help("Sets the optimization level to 3, makes your code go brrrrrr.")
            .default_value(false)
            .implicit_value(true);
    }

    cli.add_subparser(run_command);
    cli.add_subparser(build_command);

    try {
        cli.parse_args(argc, argv);
    }
    catch (const std::exception& err) {
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