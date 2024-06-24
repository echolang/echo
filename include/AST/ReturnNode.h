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
        static constexpr NodeType node_type = NodeType::n_func_return;

        ExprNode *expr = nullptr;

        ReturnNode(ExprNode *expr) : expr(expr) {};
        ~ReturnNode() {};

        const std::string node_description() override {
            return "return(" + expr->node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visitReturn(*this);
        }

    private:

    };
};

#endif