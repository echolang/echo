#ifndef SCOPEPARSER_H
#define SCOPEPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // a subscope is simply but a scope within a scope that is not a body 
    // of a function a loop or and
    AST::ScopeNode &parse_scope(Payload &payload);
};



#endif