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
    class FunctionDeclNode;

    class ExprNode : public Node
    {
    public:
        // returns the type this expression will return
        virtual ValueType result_type() const {
            return ValueType::void_type();
        }

        bool is_implcit = false;

        ExprNode() {};
        ExprNode(bool implicit) : is_implcit(implicit) {};
        virtual ~ExprNode() {};

    private:
    };

    class VoidExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expr_void;

        VoidExprNode() {};
        ~VoidExprNode() {};

        const std::string node_description() override {
            return "void";
        }

        ValueType result_type() const override {
            return ValueType::void_type();
        }

        // void goes into the void
        void accept(Visitor& visitor) override {}
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

    class VarPtrExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expr_varptr;

        VarRefNode *var_ref;

        VarPtrExprNode(VarRefNode *var_ref) :
            var_ref(var_ref)
        {};

        ~VarPtrExprNode() {};

        ValueType result_type() const override {
            assert(var_ref->decl);
            return ValueType::make_pointer(var_ref->decl->type_node()->type);
        }

        const std::string node_description() override {
            return "varptr(" + var_ref->node_description() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitVarPtrExpr(*this);
        }
    };

    class FunctionCallExprNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_expr_call;

        TokenReference token_function_name;
        std::vector<ExprNode*> arguments;

        FunctionDeclNode *decl = nullptr;

        FunctionCallExprNode(TokenReference token_function_name, std::vector<ExprNode*> arguments) :
            token_function_name(token_function_name), arguments(arguments)
        {};

        ~FunctionCallExprNode() {}

        ValueType result_type() const override;

        const std::string node_description() override {
            std::string desc = "call " + token_function_name.value() + "(";

            for (auto arg : arguments) {
                desc += arg->node_description() + ", ";
            }

            if (arguments.size() > 0) {
                desc.substr(0, desc.size() - 2);
            }

            desc += "): ";
            desc += result_type().get_type_desciption();

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
        ExprNode *lhs = nullptr;
        ExprNode *rhs = nullptr;

        BinaryExprNode(OperatorNode *op_node, ExprNode *lhs, ExprNode *rhs) :
            op_node(op_node), lhs(lhs), rhs(rhs)
        {};
        ~BinaryExprNode() {}

        ValueType result_type() const override;

        const std::string lhs_node_description() {
            return lhs ? lhs->node_description() : "[undefined]";
        }

        const std::string rhs_node_description() {
            return rhs ? rhs->node_description() : "[undefined]";
        }

        const std::string node_description() override {
            return "binexp<" + result_type().get_type_desciption() + ">(" + lhs_node_description() + " " + op_node->token_literal.value() + " " + rhs_node_description() + ")";
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