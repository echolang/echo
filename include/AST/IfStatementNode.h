#ifndef IFSTATEMENTNODE_H
#define IFSTATEMENTNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "ScopeNode.h"

#include <optional>

namespace AST 
{
    class IfStatementNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_if_statement);

        ExprNode *condition;
        ScopeNode *if_scope;
        ScopeNode *else_scope;

        // the `if` keyword, so a breakpoint set on the line the branch was written on lands on the
        // branch. optional because AST::ConstFolding and the foreach lowering both mint one
        std::optional<TokenReference> token_if;

        IfStatementNode() = default;
        IfStatementNode(
            ExprNode *condition,
            ScopeNode *if_scope,
            ScopeNode *else_scope
        ) : condition(condition), if_scope(if_scope), else_scope(else_scope) 
        {}
        ~IfStatementNode() {}

        const std::string node_description() override {
            std::string desc = "";

            desc += "if (" + condition->node_description() + ")\n";
            desc += if_scope->node_description() + "\n";

            if (else_scope) {
                desc += "else\n";
                desc += else_scope->node_description();
            }

            return desc;
        }

        void accept(Visitor &visitor) override {
            visitor.visitIfStatement(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif