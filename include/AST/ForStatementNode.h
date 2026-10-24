#ifndef FORSTATEMENTNODE_H
#define FORSTATEMENTNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ScopeNode.h"

namespace AST
{
    // `for (init; condition; step) { }`. `continue` goes to the step; a `while` continues to the
    // condition. both go through StmtCodegen::gen_loop. the init is the preceding sibling in a
    // wrapper scope, so `$i` is an ordinary frame local. reaches codegen (unlike `foreach`)
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
