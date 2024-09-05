#ifndef RETURNNODE_H
#define RETURNNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"

namespace AST 
{
    class ReturnNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_func_return);

        ExprNode *expr = nullptr;

        // the `return` keyword, so a mismatched return value has somewhere to be reported.
        // unset on the returns the struct parser synthesizes for a constructor, which carry
        // no source token of their own
        std::optional<TokenReference> token_return;

        ReturnNode(ExprNode *expr) : expr(expr) {};
        ReturnNode(ExprNode *expr, TokenReference token_return) :
            expr(expr), token_return(token_return) {};
        ~ReturnNode() {};

        const std::string node_description() override {
            if (expr == nullptr) {
                return "return(void)";
            }
            return "return(" + expr->node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visitReturn(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif