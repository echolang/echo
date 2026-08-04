#ifndef FORSTATEMENTNODE_H
#define FORSTATEMENTNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ScopeNode.h"

namespace AST
{
    // `for (init; condition; step) { }`.
    //
    // **the one loop shape whose `continue` target is not its condition**, and that is the whole of what
    // it adds. a `while` puts its step in the condition, so the back edge and a `continue` are the same
    // edge - see StmtCodegen::gen_loop, which both statements go through with this one holding a step
    // scope and a `while` holding none.
    //
    // **the init is not an edge here**, deliberately. it is the preceding sibling in a wrapper scope the
    // parser mints, and that scope *is* the init's lifetime:
    //
    //     {                                   // a scope of its own, so `$i` dies at loop exit
    //         int32 $i = 0;
    //         for (; $i < 10; $i = $i + 1) { ... }
    //     }
    //
    // an edge would need a frame that opens before the loop and closes after it, which AST::OwnershipPass
    // has no rule for; as a sibling, `$i`'s drop is the ordinary reverse-order frame drop and a `break` or
    // a `return` out of the body unwinds it through the machinery that already exists. the same shape
    // AST::ForeachLowering's `$__it` wrapper and the array literal's hoist already have
    //
    // and **not a transient node**: unlike AST::ForeachNode this reaches codegen, so it owes no
    // `body_is_concrete` arm and no PointerAdjuster throw. it owes no AST::statement_exit_kind arm either,
    // for the reason a `while` does not - a loop may run zero times
    class ForStatementNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_for_statement);

        ExprNode *condition = nullptr;

        // the step, as a scope rather than a bare statement: a temporary materialized in `$i = f($i)`
        // then dies where the step finishes, through the frame rule every other block already has
        ScopeNode *step = nullptr;

        ScopeNode *loop_scope = nullptr;

        // the `for` keyword, so a diagnostic about the statement lands on it. not optional the way
        // WhileStatementNode::token_while is - nothing mints one of these but the parser
        TokenReference token_for;

        ForStatementNode(TokenReference token_for) : token_for(token_for) {}
        ~ForStatementNode() {}

        const std::string node_description() override {
            std::string desc = "for (; ";

            desc += condition != nullptr ? condition->node_description() : "";
            desc += "; ";

            // the step's statements without the `Scope { }` around them - it holds one statement
            // written between two semicolons, and a dump reads the way the source did
            desc += step != nullptr ? step->node_description_inner() : "";
            desc += ")\n";
            desc += loop_scope != nullptr ? loop_scope->node_description() : "";
            desc += "\n";

            return desc;
        }

        void accept(Visitor &visitor) override {
            visitor.visit_for_statement(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif
