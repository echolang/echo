#ifndef ASTOPS_H
#define ASTOPS_H

#pragma once

#include "Token.h"

namespace AST
{
    enum class OpAssociativity {
        left,
        right,
        none
    };

    struct OpPrecedence {
        OpAssociativity assoc;
        int precedence;
    };

    struct Operator {
        static OpPrecedence get_precedence(const Token::Type &type);
        static TokenList shunting_yard(TokenSlice &token_slice);
    };
}

#endif