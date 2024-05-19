#include <iostream>
#include <fstream>
#include <sstream>

#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "Parser/ModuleParser.h"
#include "Compiler/CompilerException.h"
#include "Compiler/LLVM/LLVMCompiler.h"


#include <chrono>

int main() {

    // mesure performance 
    // start timer
    auto start = std::chrono::high_resolution_clock::now();

    auto bundle = AST::Bundle();

    AST::module_handle_t module_handle = bundle.modules.add_module("main");
    auto &module = bundle.modules.get_module(module_handle);

    auto parser = Parser::ModuleParser();

    parser.parse_file(std::filesystem::path("test.eco"), module, bundle.collector);

    // end timer
    auto end = std::chrono::high_resolution_clock::now();

    // dump the tokens
    auto tokeni = 0;
    for (auto &token : module.tokens.tokens) {
        auto value = module.tokens.token_values[tokeni];
        tokeni++;
        std::cout << token_type_string(token.type) << " " << value << token.line << ":" << token.char_offset << std::endl;
    }

    std::cout << "Module: " << module.debug_description() << std::endl;

    bundle.collector.print_issues();
    
    // print how long it took in milliseconds
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;

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
