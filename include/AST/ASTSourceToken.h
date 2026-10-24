#ifndef ASTSOURCETOKEN_H
#define ASTSOURCETOKEN_H

#pragma once

#include "Token.h"

namespace AST
{
    class Node;
    class ExprNode;

    // the author's token for this node, or null when there is none (a synthesized release, a scope).
    // the switch has no default. location_of_expression asserts for expressions
    const TokenReference *source_token_of(const Node &node);

    const TokenReference &location_of_expression(ExprNode *expr);
};

#endif
