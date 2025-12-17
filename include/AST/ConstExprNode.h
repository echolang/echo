#ifndef CONSTEXPRNODE_H
#define CONSTEXPRNODE_H

#pragma once

#include "AST/ExprNode.h"

namespace AST
{
    // `const(<expression>)` - a value the compiler is **required** to work out for itself.
    //
    // **transient**, exactly as AST::ConstIfNode is: AST::ConstFolding replaces it with the literal it
    // folded to, inside the monomorphizer's fixpoint. so nothing after that point has an arm for it, and
    // the two obligations that fail silently are the same two - body_is_concrete answering false while one
    // is present, and PointerAdjuster throwing for a survivor.
    //
    // **the marker is the feature.** an expression the compiler happens to be able to fold is folded by
    // nothing today and does not need to be - LLVM does that better, and notes/constants.md is emphatic
    // that a constant is its expression rather than a value. what has no spelling without this is the
    // *demand*: "work this out now, and say so if you cannot". so a `const(...)` whose operand does not
    // fold is a located error rather than an expression that quietly runs.
    //
    // **transparent while it is here.** result_type() is the operand's, so the type every surrounding rule
    // reads is the type the folded literal will have - which is what lets a `const(...)` sit anywhere an
    // expression sits without a single arm being taught about it. it carries no type of its own to
    // disagree with the operand's, and there is deliberately no `folded` flag: the replacement is the
    // idempotency, and one still in an edge is still undecided
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
