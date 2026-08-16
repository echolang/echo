#ifndef USEDECLNODE_H
#define USEDECLNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "Lexer.h"

namespace AST
{
    // a file-scope `use` statement. the bindings themselves live on the File - this node is
    // the statement in the tree, so `-p ast` can show it and first_top_level_statement can
    // skip it. it emits nothing
    class UseDeclNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_use_decl);

        TokenSlice tokens;
        std::string spelling;

        UseDeclNode(const TokenSlice &tokens, std::string spelling) :
            tokens(tokens), spelling(std::move(spelling))
        {};

        ~UseDeclNode() {};

        const std::string node_description() override {
            return spelling;
        }

        void accept(Visitor &visitor) override {
            visitor.visit_use_decl(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
