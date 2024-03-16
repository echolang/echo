#include <iostream>
#include <fstream>
#include <sstream>

#include "Lexer.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "Parser/ModuleParser.h"

int main() {

    auto module = AST::Module();
    auto parser = Parser::ModuleParser();
    auto collector = AST::Collector();

    parser.parse_file(std::filesystem::path("test.eco"), module, collector);
    
    return 0;
}
