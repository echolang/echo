#ifndef EXPRESSIONNODE_H
#define EXPRESSIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Token.h"

#include "VarRefNode.h"

#include <optional>

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

        std::optional<ValueTypePrimitive> expected_primitive_type;

        LiteralPrimitiveExprNode(TokenReference token) :
            token_literal(token)
        {
        };

        LiteralPrimitiveExprNode(TokenReference token, ValueTypePrimitive expected) :
            token_literal(token),
            expected_primitive_type(expected)
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
            return token_literal.value().back() != 'f';
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralFloatExpr(*this);
        }

        std::string get_fvalue_string() const {
            if (is_double_precision()) {
                return token_literal.value();
            } else {
                return token_literal.value().substr(0, token_literal.value().size() - 1);
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
        LiteralIntExprNode(TokenReference token) :
            LiteralPrimitiveExprNode(token)
        {};

        ValueType result_type() const override {
            return ValueType(ValueTypePrimitive::t_int32);
        }

        void accept(Visitor& visitor) override {
            visitor.visitLiteralIntExpr(*this);
        }

        int32_t int32_value() const {
            return std::stoi(token_literal.value());
        }

        int64_t int64_value() const {
            return std::stoll(token_literal.value());
        }

        uint32_t uint32_value() const {
            return std::stoul(token_literal.value());
        }

        uint64_t uint64_value() const {
            return std::stoull(token_literal.value());
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

    class VarRefExprNode : public ExprNode
    {
    public:
        VarRefNode *var_ref;

        VarRefExprNode(VarRefNode *var_ref) :
            var_ref(var_ref)
        {};

        ~VarRefExprNode() {};

        ValueType result_type() const override {
            assert(var_ref->decl);
            return var_ref->decl->type_node()->type;
        }

        const std::string node_description() override {
            return "varexp(" + var_ref->node_description() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitVarRefExpr(*this);
        }
    };

    class FunctionCallExprNode : public ExprNode
    {
    public:
        TokenReference token_function_name;
        std::vector<ExprNode*> arguments;

        FunctionCallExprNode(TokenReference token_function_name, std::vector<ExprNode*> arguments) :
            token_function_name(token_function_name), arguments(arguments)
        {};

        ~FunctionCallExprNode() {}

        const std::string node_description() override {
            std::string desc = "call " + token_function_name.value() + "(";

            for (auto arg : arguments) {
                desc += arg->node_description() + ", ";
            }

            desc += ")";

            return desc;
        }

        void accept(Visitor& visitor) override {
            visitor.visitFunctionCallExpr(*this);
        }
    };

};

#endif