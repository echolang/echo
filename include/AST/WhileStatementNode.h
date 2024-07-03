#ifndef WHILESTATEMENTNODE_H
#define WHILESTATEMENTNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "ScopeNode.h"

namespace AST 
{
    class WhileStatementNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_while_statement;

        ExprNode *condition;
        ScopeNode *loop_scope;

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

    private:

    };
};

#endif