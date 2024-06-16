#ifndef OPERATORNODE_H
#define OPERATORNODE_H

#pragma once

#include "ASTNode.h"
#include "../Lexer.h"

namespace AST 
{
    struct Operator;

    class OperatorNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_operator;
        
        TokenReference token_literal;
        const Operator *op;

        OperatorNode(TokenReference token, const Operator *op) :
            token_literal(token), op(op)
        {
        };

        ~OperatorNode() {};

        const std::string node_description() override {
            return "operator<" + token_literal.value() + ">";
        }

        void accept(Visitor& visitor) override {
            visitor.visitOperator(*this);
        }
    };
};

#endif