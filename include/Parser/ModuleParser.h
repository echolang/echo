#ifndef MODULEPARSER_H
#define MODULEPARSER_H

#pragma once

#include "ParserCursor.h"
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

        void parse(Cursor &cursor, AST::Module &module) const;

        void parse_file(std::filesystem::path path, AST::Module &module, AST::Collector &collector) const;
    };
};

#endif