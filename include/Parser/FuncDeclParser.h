#ifndef FUNCDECLPARSER_H
#define FUNCDECLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    void parse_funcdecl(Payload &payload);
};


#endif