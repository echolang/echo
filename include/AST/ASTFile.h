#ifndef ASTFILE_H
#define ASTFILE_H

#pragma once

#include <filesystem>

#include "../Token.h"
#include "ScopeNode.h"

namespace AST
{  
    class File
    {
        const std::filesystem::path path;

    public:
        File(
            const std::filesystem::path &path,
            ScopeNode &root
        ) : 
            path(path), 
            root(root) 
        {};
        ~File() {};
        
        ScopeNode &root;
    };
};
#endif