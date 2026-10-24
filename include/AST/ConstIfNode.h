#ifndef CONSTIFNODE_H
#define CONSTIFNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ScopeNode.h"

namespace AST
{
    // `const if (<condition>) { ... }`. ConstFolding splices the taken arm into the tree; the
    // other is discarded. a runtime `if` over a folded constant still ownership-walks the dead
    // arm, so this is its own node. body_is_concrete is false while one is present;
    // PointerAdjuster::visit_const_if throws. not an expression
    class ConstIfNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_const_if);

        ExprNode *condition = nullptr;
        ScopeNode *if_scope = nullptr;
        ScopeNode *else_scope = nullptr;

        // **the `const`, not the `if`.** it is the token that made the promise every refusal about this
        // node is about, and the one a reader would delete to make the branch a runtime one
        TokenReference token_const;

        // **no `lowered` flag**, for AST::ForeachNode's reason: every terminal path reseats this node's
        // edge in its parent scope, so the removal *is* the idempotency - one still in a child list is by
        // construction still undecided

        ConstIfNode(TokenReference token_const) : token_const(token_const) {}
        ~ConstIfNode() {}

        const std::string node_description() override {
            std::string desc = "const if (";
            desc += condition != nullptr ? condition->node_description() : "[none]";
            desc += ")\n";
            desc += if_scope != nullptr ? if_scope->node_description() : "[no arm]";

            if (else_scope != nullptr) {
                desc += "\nelse\n" + else_scope->node_description();
            }

            return desc;
        }

        void accept(Visitor &visitor) override {
            visitor.visit_const_if(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
