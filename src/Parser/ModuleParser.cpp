#include "Parser/ModuleParser.h"

#include "Compiler/PhaseTimings.h"
#include "Compiler/ProgressReporter.h"

#include "eco.h"
#include "Parser/ConditionalFilter.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>

Parser::ModuleParser::ModuleParser(Compiler::TargetFacts facts, std::set<std::string> test_modules) :
    target_facts(std::move(facts)), test_modules(std::move(test_modules))
{
    _lexer = std::make_unique<Lexer>();
}

Compiler::TargetFacts Parser::ModuleParser::facts_for(const AST::Module &module) const
{
    return Compiler::TargetFacts::for_module(target_facts, module.name, test_modules);
}

Parser::Payload Parser::ModuleParser::make_parser_payload(
    const AST::TokenizedFile &tfile,
    AST::Module &module,
    AST::Collector &collector,
    Parser::Pass pass
) const
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
        collector,
        pass
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
    // the conditional filter is handed *in* rather than called by Module::tokenize, so that AST knows only
    // that something may shorten the range before the slice is taken - see AST::Module::TokenFilter
    const Compiler::TargetFacts facts = facts_for(module);

    return module.tokenize(*_lexer.get(), file,
        [&facts](TokenCollection &tokens, size_t from, std::string &out_error) {
            return Parser::filter_conditional_tokens(tokens, from, facts, out_error);
        });
}

void Parser::ModuleParser::parse_module(AST::Module &module, AST::Collector &collector) const
{
    // build all parser payloads
    std::vector<std::tuple<AST::File *, AST::TokenizedFile>> file_payloads;
    {
    Compiler::ScopedPhase phase("lex");
    for (auto &file : module.files()) {
        // **the one loop that visits each file exactly once**, which is why "which file are we on" is
        // answerable here and nowhere else in this function: the three passes below each walk the whole
        // list again, so a tick in one of them would describe a file already read. It reaches a
        // process-wide reporter from the parser exactly as the ScopedPhase above it does
        Compiler::ProgressReporter::instance().tick(file.get_path().filename().string());

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
    }

    // three passes over the same tokens, each one only naming what the next one needs
    //
    // the split exists because a declaration can name anything the module declares, in any file and
    // on any line: a type name has to be known before a signature that mentions it is read, and a
    // signature has to be registered before a body that calls it is read. one pass per level of that
    // dependency, applied to *every* file before the next level starts, is what makes the whole
    // thing independent of the order the files were given on the command line

    // types first: just the names, so that a property or a parameter can be typed by a struct
    // declared further down or in another file
    {
    Compiler::ScopedPhase phase("pass 1: type names");
    for (auto &[file, tfile] : file_payloads) {
        auto parser_payload = make_parser_payload(tfile, module, collector, Pass::t_type_names);
        parse_type_names(parser_payload);
    }
    }

    // then the declaration surface - function, method and constructor signatures, and struct
    // properties - so that a call can resolve against a declaration written anywhere
    {
    Compiler::ScopedPhase phase("pass 2: declarations");
    for (auto &[file, tfile] : file_payloads) {
        auto parser_payload = make_parser_payload(tfile, module, collector, Pass::t_declarations);
        parse_symbols(parser_payload);
    }
    }

    // dump symbols
    if (dump_symbols) {
        std::cout << collector.namespaces.root().debug_dump_symbols() << std::endl;
    }

    // and finally the bodies
    {
    Compiler::ScopedPhase phase("pass 3: bodies");
    for (auto &[file, tfile] : file_payloads) {
        auto parser_payload = make_parser_payload(tfile, module, collector, Pass::t_bodies);
        file->root = &parse_scope(parser_payload);
    }
    }
}

void Parser::ModuleParser::parse_input(const InputPayload &payload) const
{
    {
        Compiler::ScopedPhase read_phase("read sources");

        for (const auto &input_file : payload.files) {
            if (input_file.content.has_value()) {
                parse_file_from_mem(input_file.path, input_file.content.value(), payload.module, payload.collector);
            } else {
                parse_file_from_disk(input_file.path, payload.module, payload.collector);
            }
        }
    }


    parse_module(payload.module, payload.collector);
}
