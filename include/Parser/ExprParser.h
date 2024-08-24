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

    // parses a chain of `->member` accesses onto `base`, wrapping it in one
    // MemberAccessNode per level. returns `base` unchanged when there is no `->`.
    // on a malformed chain (missing identifier after `->`) it collects an
    // UnexpectedToken issue and returns make_void_ref(); callers do their own recovery
    const AST::NodeReference parse_member_chain(Payload &payload, AST::NodeReference base);
};

#endif