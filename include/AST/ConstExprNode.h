#ifndef CONSTEXPRNODE_H
#define CONSTEXPRNODE_H

#pragma once

#include "AST/ExprNode.h"

namespace AST
{
    // `const(<expression>)`: fold now, or a located error. transient; ConstFolding replaces it with
    // the literal. result_type() is the operand's
    class ConstExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_const);

        ExprNode *operand = nullptr;

        // the `const`, for the same reason ConstIfNode keeps it: it is what a refusal is about
        TokenReference token_const;

        ConstExprNode(TokenReference token_const, ExprNode *operand)
            : operand(operand), token_const(token_const)
        {}

        ~ConstExprNode() {}

        ValueType result_type() const override {
            return operand != nullptr ? operand->result_type() : ValueType::make_unknown();
        }

        const std::string node_description() override {
            return "const(" + (operand != nullptr ? operand->node_description() : "[none]") + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visit_const_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
