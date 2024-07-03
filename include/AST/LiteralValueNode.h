#ifndef LITERALVALUENODE_H
#define LITERALVALUENODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "../Lexer.h"

namespace AST 
{
    class LiteralPrimitiveExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_literal;

        TokenReference token_literal;

        std::optional<ValueTypePrimitive> expected_primitive_type;

        std::optional<std::string> override_literal_value;

        LiteralPrimitiveExprNode(TokenReference token) :
            token_literal(token)
        {
        };

        LiteralPrimitiveExprNode(TokenReference token, ValueTypePrimitive expected) :
            token_literal(token),
            expected_primitive_type(expected)
        {
        };

        const std::string effective_token_literal_value() const {
            return override_literal_value.value_or(token_literal.value());
        }

        const std::string node_description() override {
            return "literal<" + result_type().get_type_desciption() + ">(" + effective_token_literal_value() + ")";
        }
    };

    class LiteralFloatExprNode : public LiteralPrimitiveExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_literal_float;
        
        LiteralFloatExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        LiteralFloatExprNode(TokenReference token, ValueTypePrimitive expected) :
            LiteralPrimitiveExprNode(token, expected)
        {
            assert(expected == ValueTypePrimitive::t_float64 || expected == ValueTypePrimitive::t_float32);
        };

        ValueTypePrimitive get_effective_primitive_type() const {
            return expected_primitive_type.value_or(
                is_double_precision() ? ValueTypePrimitive::t_float64 : ValueTypePrimitive::t_float32
            );
        }

        ValueType result_type() const override {
            return ValueType(get_effective_primitive_type());
        }

        // floats literals have to end with a "f" to be considered a float
        // everything else is considered a double
        bool is_double_precision() const {
            return effective_token_literal_value().back() != 'f';
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralFloatExpr(*this);
        }

        std::string get_fvalue_string() const {
            if (is_double_precision()) {
                return effective_token_literal_value();
            } else {
                return effective_token_literal_value().substr(0, token_literal.value().size() - 1);
            }
        }


        float float_value() const {
            assert(get_effective_primitive_type() == ValueTypePrimitive::t_float32);
            // cut off the "f" at the end
            return std::stof(get_fvalue_string());
        }

        double double_value() const {
            assert(get_effective_primitive_type() == ValueTypePrimitive::t_float64);
            return std::stod(get_fvalue_string());
        }
    };
    
    class LiteralIntExprNode : public LiteralPrimitiveExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_literal_int;

        LiteralIntExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        LiteralIntExprNode(TokenReference token, ValueTypePrimitive expected) :
            LiteralPrimitiveExprNode(token, expected)
        {
            assert(
                expected == ValueTypePrimitive::t_int8 ||
                expected == ValueTypePrimitive::t_int16 ||
                expected == ValueTypePrimitive::t_int32 ||
                expected == ValueTypePrimitive::t_int64 ||
                expected == ValueTypePrimitive::t_uint8 ||
                expected == ValueTypePrimitive::t_uint16 ||
                expected == ValueTypePrimitive::t_uint32 ||
                expected == ValueTypePrimitive::t_uint64
            );
        };

        ValueType result_type() const override {
            return ValueType(expected_primitive_type.value_or(ValueTypePrimitive::t_int32));
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralIntExpr(*this);
        }

        int8_t int8_value() const {
            return std::stoi(effective_token_literal_value());
        }

        int16_t int16_value() const {
            return std::stoi(effective_token_literal_value());
        }

        int32_t int32_value() const {
            return std::stoi(effective_token_literal_value());
        }

        int64_t int64_value() const {
            return std::stoll(effective_token_literal_value());
        }

        uint8_t uint8_value() const {
            return std::stoul(effective_token_literal_value());
        }

        uint16_t uint16_value() const {
            return std::stoul(effective_token_literal_value());
        }

        uint32_t uint32_value() const {
            return std::stoul(effective_token_literal_value());
        }

        uint64_t uint64_value() const {
            return std::stoull(effective_token_literal_value());
        }
    };

    class LiteralBoolExprNode : public LiteralPrimitiveExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_literal_bool;
    
        LiteralBoolExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        bool get_bool_value() const {
            return token_literal.value() == "true";
        }

        ValueType result_type() const override {
            return ValueType(ValueTypePrimitive::t_bool);
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralBoolExpr(*this);
        }
    };
};

#endif