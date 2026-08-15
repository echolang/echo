#ifndef VARDECLPARSER_H
#define VARDECLPARSER_H

#pragma once


#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // **`allows_guard` is asked of the walk, not of the declaration.** `T $x = guard <v> else { ... }`
    // is a statement that *ends in a block*, so it may only appear where a statement may - which is one
    // of this function's five callers. the other four read a declaration in a position no block can
    // follow: a parameter list, a struct body, and a `for` header's init and step. the same shape the
    // `static`-in-a-body refusal has, and for the same reason - the caller is what knows where it is
    AST::VarDeclNode *parse_varexpr(
        Payload &payload,
        AST::ScopeNode *scope = nullptr,
        bool allows_guard = false
    );

    // **the value an assignment writes into `target`.** called with the `=` seen but not consumed, and
    // answers null when either question below refuses - having already left the cursor at the next
    // statement, so a caller only has to stop
    //
    // two askers, and the only thing that differs between them is what the target chain is rooted in: a
    // `$var` (Parser::parse_varexpr) or a call (Parser::finish_place_statement). the two questions are
    // the same in both - **can this be written to**, reported here on the `=` rather than left to the
    // lvalue codegen's locationless "not addressable" throw, and **at what type**, which is the type the
    // *storage* holds so a pointer target writes through it rather than being re-seated
    // (re-seating is spelled `:$`, writing through is the deref)
    //
    // one function because the alternative was two, which is how the day one of them grows a rule the
    // other wants it ends with only one of them having it
    AST::ExprNode *parse_assigned_value(
        Payload &payload,
        AST::ExprNode *target,
        const TokenReference &assign_token
    );
};

#endif
