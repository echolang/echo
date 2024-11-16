#ifndef SCOPEPARSER_H
#define SCOPEPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // parses statements until the scope's closing brace. when `into` is given the statements are
    // appended to that node instead of a fresh one, which is how a caller seeds a body with a
    // declaration of its own - a constructor's `$this`. that declaration has to be the *first*
    // child: allocas are emitted in child order and a clone rebinds in child order, so one
    // appended afterwards is both un-alloca'd at its first use and rebound to the template's decl
    AST::ScopeNode &parse_scope(Payload &payload, AST::ScopeNode *into = nullptr);

    // the tail of a call used as a statement: appends it to `scope` and consumes the semicolon that
    // has to follow. shared by the free-call branch of parse_scope and the `$obj->m();` branch of
    // parse_varexpr, so the two cannot disagree about the terminator - they already did, one
    // checking t_semicolon by hand and the other going through the vardecl end-token rules
    void finish_call_statement(Payload &payload, AST::ScopeNode &scope, AST::ExprNode *call);
};



#endif