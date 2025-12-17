#ifndef WHILESTATEMENTNODE_H
#define WHILESTATEMENTNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "AST/ScopeNode.h"

#include <optional>

namespace AST
{
    class WhileStatementNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_while_statement);

        ExprNode *condition;
        ScopeNode *loop_scope;

        // the `while` keyword - see IfStatementNode::token_if. AST::ForeachLowering mints one of these
        // with no keyword behind it, which is why it is optional
        std::optional<TokenReference> token_while;

        WhileStatementNode() = default;
        WhileStatementNode(
            ExprNode *condition,
            ScopeNode *loop_scope
        ) : condition(condition), loop_scope(loop_scope)
        {}
        ~WhileStatementNode() {}

        const std::string node_description() override {
            std::string desc = "";

            desc += "while (" + condition->node_description() + ")\n";
            desc += loop_scope->node_description() + "\n";
            return desc;
        }

        void accept(Visitor &visitor) override {
            visitor.visitWhileStatement(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif
