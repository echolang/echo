#ifndef EXPRESSIONNODE_H
#define EXPRESSIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Token.h"

#include "OperatorNode.h"

#include <optional>

namespace AST
{
    class FunctionDeclNode;
    class VarRefNode;
    class TypeNode;

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
        ECO_AST_NODE_TYPE(n_expr_void);

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

        Node *clone(CloneContext &cc) const override;
    };

    class FunctionCallExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_call);

        TokenReference token_function_name;
        std::vector<ExprNode*> arguments;

        // explicit type arguments written at the call site, e.g. foo<int>(...). Empty when the
        // call relies on inference. The monomorphizer prefers these over inferred type args.
        std::vector<TypeNode*> explicit_type_args;

        FunctionDeclNode *decl = nullptr;

        FunctionCallExprNode(TokenReference token_function_name, std::vector<ExprNode*> arguments) :
            token_function_name(token_function_name), arguments(arguments)
        {};

        ~FunctionCallExprNode() {}

        ValueType result_type() const override;

        const std::string decorated_func_name() const;

        const std::string node_description() override;

        void accept(Visitor& visitor) override {
            visitor.visitFunctionCallExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    class BinaryExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_binary);

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

        Node *clone(CloneContext &cc) const override;
    };

    class UnaryExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_unary);

        TokenReference token_operator;

        ExprNode *expr;

        UnaryExprNode(TokenReference token_operator, ExprNode *expr) :
            token_operator(token_operator), expr(expr)
        {};

        ~UnaryExprNode() {}

        // negation preserves the operand type
        ValueType result_type() const override {
            return expr->result_type();
        }

        const std::string node_description() override {
            return "unexp(" + token_operator.value() + expr->node_description() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitUnaryExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    class VarPtrExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_varptr);

        VarRefNode *var_ref;

        VarPtrExprNode(VarRefNode *var_ref) :
            var_ref(var_ref)
        {
            assert(var_ref != nullptr && "VarPtrExprNode requires a valid VarRefNode");
        };

        ~VarPtrExprNode() {}

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor& visitor) override {
            visitor.visitVarPtrExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

};

#endif