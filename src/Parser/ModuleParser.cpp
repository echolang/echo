#include "Parser/ModuleParser.h"

#include "eco.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>

Parser::ModuleParser::ModuleParser()
{
    _lexer = std::make_unique<Lexer>();
}

Parser::Payload Parser::ModuleParser::make_parser_payload(const AST::TokenizedFile &tfile, AST::Module &module, AST::Collector &collector) const 
{
    auto cursor = Cursor(module.tokens, tfile.token_slice.start_index, tfile.token_slice.end_index);

    AST::Context context = {
        .module = module,
        .file = tfile,
        .current_namespace = &collector.namespaces.root(),
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

    // // parse the file
    // auto &tfile = make_tokenized_file(module, file);
    // auto payload = make_parser_payload(tfile, module, collector);

    // // begin parsing the file root
    // file.root = &Parser::parse_scope(payload);   
}

void Parser::ModuleParser::parse_file_from_mem(std::filesystem::path path, const std::string &content, AST::Module &module, AST::Collector &collector) const
{
    // create a file entry in the module
    auto &file = module.add_file(path);
    
    // set the content of the file
    file.set_content(content);

    // ensure the content is set
    assert(file.content.has_value());

    // // parse the file
    // auto &tfile = make_tokenized_file(module, file);
    // auto payload = make_parser_payload(tfile, module, collector);

    // // begin parsing the file root
    // file.root = &Parser::parse_scope(payload);   
}

AST::TokenizedFile Parser::ModuleParser::make_tokenized_file(AST::Module &module, AST::File &file) const
{
    return module.tokenize(*_lexer.get(), file);
}

void Parser::ModuleParser::parse_module(AST::Module &module, AST::Collector &collector) const
{
    // build all parser payloads
    std::vector<std::tuple<AST::File *, AST::TokenizedFile>> file_payloads;
    for (auto &file : module.files()) {
#if ECO_DONT_CATCH_EXCEPTIONS 
        auto tfile = make_tokenized_file(module, file);
        file_payloads.push_back(std::make_tuple(&file, tfile));
#else
        try {
            auto tfile = make_tokenized_file(module, file);
            file_payloads.push_back(std::make_tuple(&file, tfile));
        }
        catch (const Lexer::TokenException &e) {
            throw TokenizationException(e, &file);
        }
#endif
    }

    // first pass to find declared symbols (functions, types) so that we can 
    // reference them when actually parsing the code
    for (auto &file_payload : file_payloads) {
        auto parser_payload = make_parser_payload(std::get<1>(file_payload), module, collector);
        parse_symbols(parser_payload);
    }

    // dump symbols
    if (dump_symbols) {
        std::cout << collector.namespaces.root().debug_dump_symbols() << std::endl;
    }

    // second pass to actually parse the code
    for (auto &file_payload : file_payloads) {
        auto file = std::get<0>(file_payload);
        auto parser_payload = make_parser_payload(std::get<1>(file_payload), module, collector);
        file->root = &parse_scope(parser_payload);
    }
}

void Parser::ModuleParser::parse_input(const InputPayload &payload) const
{
    for (const auto &input_file : payload.files) {
        if (input_file.content.has_value()) {
            parse_file_from_mem(input_file.path, input_file.content.value(), payload.module, payload.collector);
        } else {
            parse_file_from_disk(input_file.path, payload.module, payload.collector);
        }
    }
    
    parse_module(payload.module, payload.collector);
}