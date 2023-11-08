#ifndef ASTMODULE_H
#define ASTMODULE_H

#pragma once

#include "ASTNode.h"
#include "ASTNodeReference.h"
#include "ScopeNode.h"

namespace AST
{   
    class Module
    {
    public:
        NodeCollection nodes = NodeCollection();
        ScopeNode &root; 

        Module() : root(nodes.emplace_back<ScopeNode>()) {}
        ~Module() {}

    private:

    };

};
#endif