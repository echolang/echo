#ifndef MODULEPARSER_H
#define MODULEPARSER_H

#pragma once

#include "ParserCursor.h"
#include "ParserPayload.h"
#include "../Token.h"
#include "../Lexer.h"
#include "../AST/ASTModule.h"
#include "../AST/ASTCollector.h"

#include <memory>

namespace Parser
{
    class ModuleParser
    {
        std::unique_ptr<Lexer> _lexer;

    public:
        ModuleParser();
        ~ModuleParser() {};
        
        AST::TokenizedFile &make_tokenized_file(AST::Module &module, AST::File &file) const;

        Parser::Payload make_parser_payload(const AST::TokenizedFile &file, AST::Module &module, AST::Collector &collector) const;

        void parse_file_from_disk(
            std::filesystem::path path, 
            AST::Module &module, 
            AST::Collector &collector
        ) const;

        void parse_file_from_mem(
            std::filesystem::path path,
            const std::string &content,
            AST::Module &module, 
            AST::Collector &collector
        ) const;

    private:
    };
};

#endif