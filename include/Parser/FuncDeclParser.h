#ifndef FUNCDECLPARSER_H
#define FUNCDECLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTContext.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // which grammar a `function` declaration is being read under. an extern declaration accepts
    // the `function <c_symbol> as <echo_name>(...)` renaming spelling and must be bodyless;
    // everything else about the signature is parsed by the same code, so the two forms cannot
    // drift apart
    enum class FuncDeclKind {
        t_normal,
        t_extern,
    };

    AST::FunctionDeclNode *parse_funcdecl(Payload &payload, bool symbol_only = false, FuncDeclKind kind = FuncDeclKind::t_normal);
};


#endif