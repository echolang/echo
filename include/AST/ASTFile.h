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
        const TokenCollection::Slice tokens_slice;

    public:

        ScopeNode *root = nullptr;
        
        File(
            const std::filesystem::path &path,
            const TokenCollection::Slice &tokens_slice
        ) : 
            path(path), 
            tokens_slice(tokens_slice)
        {};
        ~File() {};

        const std::filesystem::path &get_path() const {
            return path;
        }

        std::string debug_description() const;
    };
};
#endif