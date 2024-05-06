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

void Parser::ModuleParser::parse_file(std::filesystem::path path, AST::Module &module, AST::Collector &collector) const
{
    // create a file entry in the module
    auto &file = module.add_file(path);
    file.read_from_disk();

    assert(file.content.has_value());

    auto &tfile = module.tokenize(*_lexer, file);

    auto cursor = Cursor(module.tokens, tfile.token_slice.start, tfile.token_slice.end);

    AST::Context context = {
        .module = module,
        .file = tfile
    };

    auto payload = Payload {
        cursor,
        context,
        collector
    };

    file.root = &Parser::parse_scope(payload);

    parse(cursor, module);
}
