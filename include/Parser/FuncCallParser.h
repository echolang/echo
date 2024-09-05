#ifndef FUNCCALLPARSER_H
#define FUNCCALLPARSER_H

#pragma once

#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::FunctionCallExprNode *parse_funccall(Parser::Payload &payload, const AST::Namespace *requested_namespace = nullptr);

    // true when the cursor sits on a call used as a statement: an optionally namespace-qualified
    // name followed by `(` or by explicit type arguments. `mem::free($p);` and `box<int32>(1);`
    // are both calls that the bare `{identifier, open_paren}` test misses, which left the
    // statement form of every qualified and every explicitly-parameterised call unparseable
    bool starts_call_statement(Parser::Payload &payload);
};

#endif