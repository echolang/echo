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
    AST::FunctionDeclNode *parse_funcdecl(Payload &payload, bool symbol_only = false);
};


#endif