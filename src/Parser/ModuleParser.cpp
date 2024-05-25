#include "Parser/ModuleParser.h"

#include "Parser/ScopeParser.h"

#include <iostream>
#include <fstream>
#include <sstream>

Parser::ModuleParser::ModuleParser()
{
    _lexer = std::make_unique<Lexer>();
}

Parser::Payload Parser::ModuleParser::make_parser_payload(const AST::TokenizedFile &tfile, AST::Module &module, AST::Collector &collector) const 
{
    auto cursor = Cursor(module.tokens, tfile.token_slice.start_index, tfile.token_slice.end_index);

    AST::Context context = {
        .module = module,
        .file = tfile
    };

    return Payload {
        cursor,
        context,
        collector
    };
}

void Parser::ModuleParser::parse_file_from_disk(std::filesystem::path path, AST::Module &module, AST::Collector &collector) const
{
    // create a file entry in the module
    auto &file = module.add_file(path);
    file.read_from_disk();

    // ensure the content is set
    assert(file.content.has_value());

    // parse the file
    auto &tfile = make_tokenized_file(module, file);
    auto payload = make_parser_payload(tfile, module, collector);

    // begin parsing the file root
    file.root = &Parser::parse_scope(payload);   
}

void Parser::ModuleParser::parse_file_from_mem(std::filesystem::path path, const std::string &content, AST::Module &module, AST::Collector &collector) const
{
    // create a file entry in the module
    auto &file = module.add_file(path);
    
    // set the content of the file
    file.set_content(content);

    // ensure the content is set
    assert(file.content.has_value());

    // parse the file
    auto &tfile = make_tokenized_file(module, file);
    auto payload = make_parser_payload(tfile, module, collector);

    // begin parsing the file root
    file.root = &Parser::parse_scope(payload);   
}

AST::TokenizedFile &Parser::ModuleParser::make_tokenized_file(AST::Module &module, AST::File &file) const
{
    auto &tfile = module.tokenize(*_lexer, file);
    return tfile;
}
