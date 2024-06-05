#ifndef OPERATORNODE_H
#define OPERATORNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "../Lexer.h"

namespace AST 
{
    class OperatorNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_operator;

        TokenReference token_literal;

        OperatorNode(TokenReference token) :
            token_literal(token)
        {
        };

        const std::string node_description() override {
            return "operator<" + token_literal.value() + ">";
        }
    };
};

#endif