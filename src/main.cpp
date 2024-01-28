#include <iostream>
#include <fstream>
#include <sstream>

#include "Lexer.h"
#include "AST/ASTModule.h"
#include "Parser/ModuleParser.h"

int main() {

    auto module = AST::Module();
    auto parser = Parser::ModuleParser();

    parser.parse_file(std::filesystem::path("test.eco"), module);
    
    return 0;
}
