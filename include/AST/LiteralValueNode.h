#ifndef LITERALVALUENODE_H
#define LITERALVALUENODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "Lexer.h"

namespace AST 
{
    class LiteralPrimitiveExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_literal);

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
            auto effective_value = effective_token_literal_value();
            if (effective_value != token_literal.value()) {
                return "literal<" + result_type().get_type_desciption() + ">(" + effective_value + " [" + token_literal.value() + "])";
            }

            return "literal<" + result_type().get_type_desciption() + ">(" + effective_value + ")";
        }
    };

    class LiteralFloatExprNode : public LiteralPrimitiveExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_literal_float);
        
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

        void accept(Visitor &visitor) override {
            visitor.visitLiteralFloatExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;

        std::string get_fvalue_string() const {
            const std::string value = effective_token_literal_value();

            if (is_double_precision()) {
                return value;
            }

            // cut off the trailing "f". the length has to be measured on the effective value: an
            // autocast literal carries an override that is longer than the source token it came
            // from ("0.25" becomes "0.25f"), and measuring the token instead cut one character
            // too many - a float32 0.25 reached codegen as 0.2
            return value.substr(0, value.size() - 1);
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
        ECO_AST_NODE_TYPE(n_literal_int);

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
                expected == ValueTypePrimitive::t_uint64 ||
                expected == ValueTypePrimitive::t_usize ||
                expected == ValueTypePrimitive::t_isize
            );
        };

        ValueType result_type() const override {
            return ValueType(expected_primitive_type.value_or(ValueTypePrimitive::t_int32));
        }

        void accept(Visitor &visitor) override {
            visitor.visitLiteralIntExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;

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
        ECO_AST_NODE_TYPE(n_literal_bool);
    
        LiteralBoolExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        bool get_bool_value() const {
            return token_literal.value() == "true";
        }

        ValueType result_type() const override {
            return ValueType(ValueTypePrimitive::t_bool);
        }

        void accept(Visitor &visitor) override {
            visitor.visitLiteralBoolExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    class LiteralStringExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_literal_string);

        TokenReference token_literal;
    
        LiteralStringExprNode(TokenReference token) :
            token_literal(token)
        {};
        ~LiteralStringExprNode() {};

        // a C string: the address of a NUL-terminated run of bytes in the module's constants
        //
        // not the language's eventual `string` type, which does not exist yet (todo/C1) - but a
        // literal has to have *some* type for a declaration to be able to name it, which is what
        // `die(ptr<const uint8> $message)` needs. answering `unknown` meant a literal could not be
        // passed to anything and lowered to nothing at all (todo/B1)
        ValueType result_type() const override {
            // one shared instance: `make_pointer` heap-allocates its pointee, and this type does not
            // depend on the node at all, so building it per call would malloc on every overload
            // candidate scored, every adjuster visit and every fit check
            static const ValueType type = ValueType::make_pointer(
                ValueType::make_const(ValueType(ValueTypePrimitive::t_uint8)), true);
            return type;
        }

        void accept(Visitor &visitor) override {
            visitor.visitLiteralStringExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;

        const std::string node_description() override {
            return "literal<string>(\"" + token_literal.value() + "\")";
        }

        std::string get_string_value() const;
    };
};

#endif