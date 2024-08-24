#ifndef NULLNODE_H
#define NULLNODE_H

#pragma once

#include "ASTNode.h"

namespace AST 
{
    class NullNode : public Node
    {
    public:
        NullNode() {};
        ~NullNode() {};

        ECO_AST_NODE_TYPE(n_null);

        const std::string node_description() override {
            return "NULL";
        }

        void accept(Visitor &visitor) override {
            visitor.visitNull(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};


#endif