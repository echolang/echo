#include <iostream>
#include <fstream>
#include <sstream>

#include "Lexer.h"
#include "AST/ASTBundle.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "Parser/ModuleParser.h"

int main() {

    auto bundle = AST::Bundle();

    AST::module_handle_t module_handle = bundle.modules.add_module("main");
    auto &module = bundle.modules.get_module(module_handle);

    auto parser = Parser::ModuleParser();

    parser.parse_file(std::filesystem::path("test.eco"), module, bundle.collector);

    std::cout << "Module: " << module.debug_description() << std::endl;
    
    return 0;
}
