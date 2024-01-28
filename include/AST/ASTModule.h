#ifndef ASTMODULE_H
#define ASTMODULE_H

#pragma once

#include "../Token.h"
#include "ASTNode.h"
#include "ASTFile.h"
#include "ASTNodeReference.h"
#include "ScopeNode.h"

#include <filesystem>

namespace AST
{   
    class Module
    {
    public:
        TokenCollection tokens = TokenCollection();
        NodeCollection nodes = NodeCollection();

        std::vector<File> files;

        Module() {}
        ~Module() {}
        
    private:

    };

};
#endif