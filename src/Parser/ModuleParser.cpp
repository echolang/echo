#include "Parser/ModuleParser.h"

#include "Parser/ScopeParser.h"

#include <iostream>
#include <fstream>
#include <sstream>

Parser::ModuleParser::ModuleParser()
{
    _lexer = std::make_unique<Lexer>();
    
}

void Parser::ModuleParser::parse(Cursor &cursor, AST::Module &module) const
{
}

void Parser::ModuleParser::parse_file(std::filesystem::path path, AST::Module &module) const
{
    // load the file into a string
    // we probably should use a stream in the future
    auto istrm = std::ifstream(path);
    auto stream = std::stringstream();

    // if the first line is just "<?php" or "<?eco" we skip it
    // this is a TEMPORARY hack so my dump text editor will do syntax highlighting
    // without having to create a syntax highlighting extension..
    stream << istrm.rdbuf();
    auto str = stream.str();
    if (str.substr(0, 5) == "<?php" || str.substr(0, 5) == "<?eco") {
        stream.str(str.substr(5));
    }
    // end of hack
    

    size_t startindex = module.tokens.size();
    _lexer->tokenize(module.tokens, stream.str());
    size_t endindex = module.tokens.size();

    auto cursor = Cursor(module.tokens, startindex, endindex);

    AST::Context context = {
        module
    };

    auto &scope_node = Parser::parse_scope(cursor, context);
    
    module.files.emplace_back(AST::File(path, scope_node));

    parse(cursor, module);
}
