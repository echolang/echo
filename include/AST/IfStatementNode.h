#ifndef IFSTATEMENTNODE_H
#define IFSTATEMENTNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"
#include "ScopeNode.h"

namespace AST 
{
    class IfStatementNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_if_statement;
    
        struct Block {
            ExprNode *condition;
            ScopeNode *block;

            Block(ExprNode *condition, ScopeNode *block) : 
                condition(condition), 
                block(block) 
            {};
        };

        std::vector<Block> blocks;

        IfStatementNode() = default;
        IfStatementNode(std::vector<Block> blocks) : blocks(blocks) {};
        ~IfStatementNode() {}

        const std::string node_description() override {
            std::string desc = "";
            size_t i = 0;
            for (auto &block : blocks) {
                std::string block_name = "if";
                if (i > 0 && block.condition != nullptr) {
                    block_name = "else if";
                } else if (i > 0) {
                    block_name = "else";
                }

                desc += block_name;
                if (block.condition != nullptr) {
                    desc += " (" + block.condition->node_description() + ") ";
                }

                desc += "\n" + block.block->node_description() + "\n";
                i++;
            }

            return desc;
        }

        void accept(Visitor &visitor) override {
            visitor.visitIfStatement(*this);
        }

    private:

    };
};

#endif