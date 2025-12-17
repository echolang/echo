#ifndef RETURNNODE_H
#define RETURNNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"

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

        // the drops this return owes: every enclosing frame's, innermost first, since a return leaves
        // all of them at once. filled by AST::OwnershipPass.
        //
        // they ride *on* the return rather than sitting as statements ahead of it, for the same reason
        // AssignNode::teardown_old does: the returned expression may read what is being dropped -
        // `return $c->x` over an owning `$c` - so the value has to be computed first. as statements ahead
        // of the return they freed the block and then read it
        NodeReferenceList unwind;

        ReturnNode(ExprNode *expr) : expr(expr) {};
        ReturnNode(ExprNode *expr, TokenReference token_return) :
            expr(expr), token_return(token_return) {};
        ~ReturnNode() {};

        const std::string node_description() override {
            std::string buffer = expr == nullptr ? "return(void)" : "return(" + expr->node_description() + ")";

            // shown, because `-ar` is where a missing or duplicated drop is diagnosed
            for (auto &drop : unwind) {
                if (drop.has()) {
                    buffer += "\n  unwind " + drop.node()->node_description();
                }
            }

            return buffer;
        }

        void accept(Visitor &visitor) override {
            visitor.visitReturn(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif
