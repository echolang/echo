#ifndef SCOPEPARSER_H
#define SCOPEPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserCursor.h"

namespace Parser
{
    AST::ScopeNode &parse_scope(Cursor &cursor, AST::Context &context);
};



#endif