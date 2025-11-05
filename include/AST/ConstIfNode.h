#ifndef CONSTIFNODE_H
#define CONSTIFNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "ScopeNode.h"

namespace AST
{
    // `const if (<condition>) { ... } else { ... }` - a branch decided **before codegen**, whose untaken
    // arm is discarded from the tree.
    //
    // **transient.** AST::ConstFolding replaces it with the arm its condition selected, inside the
    // monomorphizer's fixpoint - so it is visible with `-a` and gone by `-ar`, and every question after
    // that point is asked about an ordinary AST::ScopeNode. no codegen arm, no ownership rule.
    // AST::ForeachNode's contract, and the same two obligations fail silently without it:
    // OwnershipPass::body_is_concrete must answer *false* while one is present, and
    // PointerAdjuster::visit_const_if must throw.
    //
    // **an ordinary `if` over a folded constant is not the same thing**, which is the whole reason this
    // exists. `if (mem::is_trivially_copyable<T>())` already emits `br i1 true` and LLVM deletes the dead
    // block under `-O`. what it cannot do is stop the dead arm from being ownership-walked, rewritten and
    // monomorphized: `clear<int32>`'s provably-dead arm declares `T $doomed` and calls `mem::take<T>`, and
    // an `array<T>`'s dead copy arm is what mints its `operator []` instances. so the marker is explicit
    // and the refusal is loud - a condition the compiler cannot answer is an error at the `const`, never a
    // runtime branch.
    //
    // deliberately **not a flag on AST::IfStatementNode**, close as the two shapes read. a flag makes the
    // failure *plausible*: a `const if` whose condition did not fold would compile as a runtime branch,
    // which is precisely the silence this exists to end - and both transient obligations would become a
    // conditional test inside an existing arm, which is the shape a forgotten check hides in. a distinct
    // node kind makes every visitor total by construction.
    //
    // deliberately **not** in NodeReference::is_expression_node(): this is a statement, beside n_scope,
    // n_if_statement and n_foreach. that trap in CLAUDE.md reads as unconditional and is not
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
