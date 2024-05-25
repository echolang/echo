#ifndef EXPRPARSER_H
#define EXPRPARSER_H

#pragma once

#include "ParserPayload.h"
#include "AST/ExprNode.h"

#include <unordered_map>

namespace Parser
{
    AST::ExprNode *parse_expr(Payload &payload, AST::TypeNode *expected_type = nullptr);
    const AST::NodeReference parse_expr_ref(Payload &payload, AST::TypeNode *expected_type = nullptr);
};

#endif