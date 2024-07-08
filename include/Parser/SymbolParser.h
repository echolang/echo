#ifndef SYMBOLPARSER_H
#define SYMBOLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    void parse_symbols(Payload &payload);
};


#endif