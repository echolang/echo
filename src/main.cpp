#include <iostream>
#include <fstream>
#include <sstream>

#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "Parser/ModuleParser.h"

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
        std::cout << token_type_string(token.type) << " " << value << std::endl;
    }

    std::cout << "Module: " << module.debug_description() << std::endl;

    bundle.collector.print_issues();
    
    // print how long it took in milliseconds
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;
    
    return 0;
}
