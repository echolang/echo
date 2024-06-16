#ifndef EXPRESSIONNODE_H
#define EXPRESSIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Token.h"

#include "VarRefNode.h"
#include "OperatorNode.h"

#include <optional>

namespace AST 
{
    class ExprNode : public Node
    {
    public:
        // returns the type this expression will return
        virtual ValueType result_type() const {
            return ValueType::void_type();
        }

        bool is_implcit = false;

        virtual ~ExprNode() {};

    private:
    };

    class VarRefExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expr_varref;

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
        static constexpr NodeType node_type = NodeType::n_expr_call;

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

    class BinaryExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expr_binary;

        OperatorNode *op_node;
        ExprNode *lhs;
        ExprNode *rhs;

        BinaryExprNode(OperatorNode *op_node, ExprNode *lhs, ExprNode *rhs) :
            op_node(op_node), lhs(lhs), rhs(rhs)
        {};

        ~BinaryExprNode() {}

        const std::string node_description() override {
            return "binexp(" + lhs->node_description() + " " + op_node->token_literal.value() + " " + rhs->node_description() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitBinaryExpr(*this);
        }
    };

    class UnaryExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expr_unary;

        TokenReference token_operator;

        ExprNode *expr;

        UnaryExprNode(TokenReference token_operator, ExprNode *expr) :
            token_operator(token_operator), expr(expr)
        {};

        ~UnaryExprNode() {}

        const std::string node_description() override {
            return "unexp(" + token_operator.value() + expr->node_description() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitUnaryExpr(*this);
        }
    };

};

#endif