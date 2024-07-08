#include <iostream>
#include <fstream>
#include <sstream>

#include <argparse.h>
#include <glob.hpp>

#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "Parser/ModuleParser.h"
#include "Compiler/CompilerException.h"
#include "Compiler/LLVM/LLVMCompiler.h"

#include <chrono>

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

int main_run(argparse::ArgumentParser &cli)
{
    auto source_files = get_file_list_from_args(cli, "source");

    // // mesure performance 
    // // start timer
    // auto start = std::chrono::high_resolution_clock::now();

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
    for (const auto& source_file : source_files) {
        input.files.push_back(Parser::ModuleParser::InputFile(source_file));
    }

    parser.parse_input(input);


    // for (const auto& source_file : source_files) {
    //     parser.parse_file_from_disk(source_file, module, bundle.collector);
    // }
    // // parser.parse_file_from_disk(std::filesystem::path("test.eco"), module, bundle.collector);

    // // end timer
    // auto end = std::chrono::high_resolution_clock::now();

    // dump the tokens
    // auto tokeni = 0;
    // for (auto &token : module.tokens.tokens) {
    //     auto value = module.tokens.token_values[tokeni];
    //     tokeni++;
    //     std::cout << token_type_string(token.type) << " " << value << token.line << ":" << token.char_offset << std::endl;
    // }

    std::cout << "Module: " << module.debug_description() << std::endl;

    bundle.collector.print_issues();
    
    // print how long it took in milliseconds
    // std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;

    if (bundle.collector.has_critical_issues()) {

        std::cout << "Critical issues found, cannot compile." << std::endl;
        return 1;
    }

    // compile the module
    LLVMCompiler compiler;

    try {
        compiler.compile_bundle(bundle);

        compiler.printIR(false);

        compiler.run_code();

        // compiler.make_exec("test");

    } catch (Compiler::CompilerException &e) {
        auto issue = &e.issue();

        std::cout << "Compiler Exception: " << e.what() << std::endl;
        std::cout << "Issue at " << issue->code_ref.token_slice.startt().line << ":" << issue->code_ref.token_slice.startt().char_offset << std::endl;
        std::cout << issue->message() << std::endl;
        std::cout << issue->code_ref.get_referenced_code_excerpt() << std::endl;
    
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
        .help(".eco source files to be parsed, compiled and run.")
        .remaining();

    cli.add_subparser(run_command);

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
    else {
        std::cerr << cli;
        return 1;
    }
}