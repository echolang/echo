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

        // returns the type this expression will return
        virtual ValueType result_type() const {
            return ValueType::void_type();
        }

        bool is_implcit = false;

        virtual ~ExprNode() {};

    private:
    };

    class LiteralPrimitiveExprNode : public ExprNode
    {
    public:
        TokenReference token_literal;

        LiteralPrimitiveExprNode(TokenReference token) :
            token_literal(token)
        {
        };

        const std::string node_description() override {
            return "literal<" + result_type().get_type_desciption() + ">(" + token_literal.value() + ")";
        }
    };

    class LiteralFloatExprNode : public LiteralPrimitiveExprNode
    {
    public:
        LiteralFloatExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        ValueType result_type() const override {
            return is_double_precision() ? ValueType(ValueTypePrimitive::t_float64) : ValueType(ValueTypePrimitive::t_float32);
        }

        // floats literals have to end with a "f" to be considered a float
        // everything else is considered a double
        bool is_double_precision() const {
            return token_literal.value().back() != 'f';
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralFloatExpr(*this);
        }

        float float_value() const {
            assert(!is_double_precision());
            // cut off the "f" at the end
            return std::stof(token_literal.value().substr(0, token_literal.value().size() - 1));
        }

        double double_value() const {
            assert(is_double_precision());
            return std::stod(token_literal.value());
        }
    };
    
    class LiteralIntExprNode : public LiteralPrimitiveExprNode
    {
    public:
        LiteralIntExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        ValueType result_type() const override {
            return ValueType(ValueTypePrimitive::t_int32);
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralIntExpr(*this);
        }
    };

    class LiteralBoolExprNode : public LiteralPrimitiveExprNode
    {
    public:
        LiteralBoolExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        ValueType result_type() const override {
            return ValueType(ValueTypePrimitive::t_bool);
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralBoolExpr(*this);
        }
    };

};

#endif