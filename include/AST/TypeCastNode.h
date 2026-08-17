#ifndef TYPECASTNODE_H
#define TYPECASTNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "Lexer.h"

#include <optional>

namespace AST
{
    class TypeCastNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_type_cast);

        ValueType cast_to;
        ExprNode *expr;

        // the `as` a written cast was spelled with, or the start of `T(...)`. empty for an implicit
        // cast the compiler inserted - those have no token of their own, and source_token_of walks
        // through to the operand. instanceof's token_instanceof is the same field
        std::optional<TokenReference> token;

        // **has AST::CastResolution answered this written cast?** implicit casts never ask, and stay
        // false. a built-in kind that stays a TypeCastNode, and a refusal that is kept in the tree,
        // both set this so nothing asks again. a declared conversion is rewritten to a call and this
        // node is gone
        bool plan_decided = false;

        TypeCastNode(ValueType cast_to, ExprNode *expr, bool implicit = false, std::optional<TokenReference> token = std::nullopt) :
            ExprNode(implicit),
            cast_to(cast_to),
            expr(expr),
            token(std::move(token))
        {};

        ValueType result_type() const override {
            return cast_to;
        }

        const std::string node_description() override {
            return "cast<" + cast_to.get_type_desciption() + ">(" + expr->node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visitTypeCast(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
