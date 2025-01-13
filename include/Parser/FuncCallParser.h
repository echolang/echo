#ifndef FUNCCALLPARSER_H
#define FUNCCALLPARSER_H

#pragma once

#include "AST/ASTCallResolution.h"
#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::FunctionCallExprNode *parse_funccall(Parser::Payload &payload, const AST::Namespace *requested_namespace = nullptr);

    // parses `->name(...)` / `->name<...>(...)` into an ordinary call whose first argument is the
    // receiver's address. the cursor must sit on the `(` or `<` that follows the member name
    //
    // a member call is not a node kind of its own: it is a FunctionCallExprNode with the receiver
    // prepended, which is why the monomorphizer, the pointer adjuster, the type checker and codegen
    // all handle it without a special case
    //
    // answers null two ways, and the caller has to tell them apart:
    //  - `is_call` false: the `<` was a comparison after all and the cursor has been restored to it,
    //    so `$a->count < 3` still parses as a member read
    //  - `is_call` true: this really was a call and it did not resolve. a diagnostic has been
    //    reported and the caller should abort rather than reinterpret the tokens
    AST::FunctionCallExprNode *parse_member_call(
        Parser::Payload &payload,
        AST::ExprNode *receiver,
        const TokenReference &member_token,
        bool &is_call);

    // true when the cursor sits on a call used as a statement: an optionally namespace-qualified
    // name followed by `(` or by explicit type arguments. `mem::free($p);` and `box<int32>(1);`
    // are both calls that the bare `{identifier, open_paren}` test misses, which left the
    // statement form of every qualified and every explicitly-parameterised call unparseable
    bool starts_call_statement(Parser::Payload &payload);
};

#endif