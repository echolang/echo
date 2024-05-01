#ifndef EXPRESSIONNODE_H
#define EXPRESSIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Token.h"

namespace AST 
{
    class ExprNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expression;

        ValueType restult_type = ValueType::void_type();

        bool is_implcit = false;

        virtual ~ExprNode() {};

    private:
    };

    class LiterlNumericExprNode : public ExprNode
    {
    public:
        TokenReference token_literal;

        // LiterlNumericExprNode(TokenReference token
        //     token_literal(token)
        // {
        //     // if (token.type() == Token::Type::t_liter
        //     // restult_type = ValueType(ValueTypePrimitive::t_uint32);
        // };

        const std::string node_description() override {
            return "literal<" + restult_type.name + ">(" + token.value() + ")";
        }
    };
};

#endif