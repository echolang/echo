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
    public:
        struct InputFile
        {
            std::filesystem::path path;
            std::optional<std::string> content;

            InputFile(std::filesystem::path path) : 
                path(path), content(std::nullopt)
            {};
            
            InputFile(std::filesystem::path path, const std::string &content) : 
                path(path), content(content)
            {};
        };

        struct InputPayload
        {
            std::vector<InputFile> files;
            AST::Module &module;
            AST::Collector &collector;
        };

        struct TokenizationException : public std::exception
        {
            Lexer::TokenException exception;
            AST::File *file;
            std::string message;

            TokenizationException(Lexer::TokenException exception, AST::File *file) : 
                exception(exception), file(file)
            {
                message = (std::string(exception.what()) + " in file " + file->get_path().string());
            };

            const char *what() const noexcept override {
                return message.c_str();
            }
        };

        // when enabled, the parser will dump all symbols to stdout
        // after parsing all files in the module
        bool dump_symbols = true;

        ModuleParser();
        ~ModuleParser() {};
        
        AST::TokenizedFile make_tokenized_file(AST::Module &module, AST::File &file) const;

        Parser::Payload make_parser_payload(const AST::TokenizedFile &file, AST::Module &module, AST::Collector &collector) const;
        
        void parse_input(const InputPayload &payload) const;

    private:
        std::unique_ptr<Lexer> _lexer;

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
    };
};

#endif