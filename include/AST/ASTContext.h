#ifndef ASTCONTEXT_H
#define ASTCONTEXT_H

#pragma once

#include "ASTModule.h"

namespace AST
{  
    struct Context
    {
        Module &module;

        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T &emplace_node(Args&&... args) {
            return module.nodes.emplace_back<T>(std::forward<Args>(args)...);
        }
    };
};
#endif