#ifndef SCOPENODE_H
#define SCOPENODE_H

#pragma once

#include "ASTNode.h"

namespace AST 
{
    class ScopeNode : public Node
    {
    public:
        NodeReferenceList children;

        ScopeNode() {};
        ~ScopeNode() {};

        static constexpr NodeType node_type = NodeType::n_scope;

        const std::string node_description() override {
            return "Scope()";
        }

    private:

    };
};

#endif