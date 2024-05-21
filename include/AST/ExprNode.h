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