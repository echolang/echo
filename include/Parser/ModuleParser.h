#ifndef MODULEPARSER_H
#define MODULEPARSER_H

#pragma once

#include "Parser/ParserCursor.h"
#include "Parser/ParserPayload.h"
#include "Token.h"
#include "Lexer.h"
#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"
#include "Compiler/TargetFacts.h"

#include <memory>
#include <exception>

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
        bool dump_symbols = false;

        // what `#[if: ...]` regions are evaluated against, for every file this parser lexes.
        //
        // a member rather than a parameter on parse_module, because it is the same for the whole
        // invocation and threading it through four call sites would invite one of them defaulting - and
        // **const, set at construction**, because a mutable one with a host default is exactly how the
        // manifest reader came to evaluate conditions against the wrong platform: it built a second parser
        // of its own, the facts were already populated, and nothing was unset enough to be noticed.
        // Compiler::TargetFacts::host() is what a caller with no command line to read asks for, and it says
        // so at the call site
        const Compiler::TargetFacts target_facts;

        explicit ModuleParser(Compiler::TargetFacts facts);
        ~ModuleParser() {};

        AST::TokenizedFile make_tokenized_file(AST::Module &module, AST::File &file) const;

        // the payload one pass walks one file with. `pass` is on the payload rather than an argument
        // to each parser, so the entry point below is the only place that has to name it
        Parser::Payload make_parser_payload(
            const AST::TokenizedFile &file,
            AST::Module &module,
            AST::Collector &collector,
            Parser::Pass pass = Parser::Pass::t_bodies) const;

        void parse_input(const InputPayload &payload) const;
        void parse_module(AST::Module &module, AST::Collector &collector) const;

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
