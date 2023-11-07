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

        static constexpr NodeType node_type = NodeType::n_null;

        const std::string node_description() override {
            return "NULL";
        }

    private:

    };
};


#endif