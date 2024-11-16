#ifndef SCOPEPARSER_H
#define SCOPEPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // parses statements until the scope's closing brace. when `into` is given the statements are
    // appended to that node instead of a fresh one, which is how a caller seeds a body with a
    // declaration of its own - a constructor's `$this`. that declaration has to be the *first*
    // child: allocas are emitted in child order and a clone rebinds in child order, so one
    // appended afterwards is both un-alloca'd at its first use and rebound to the template's decl
    AST::ScopeNode &parse_scope(Payload &payload, AST::ScopeNode *into = nullptr);
};



#endif