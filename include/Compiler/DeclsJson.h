#ifndef DECLSJSON_H
#define DECLSJSON_H

#pragma once

#include "AST/ASTBundle.h"

#include <string>
#include <unordered_set>

namespace Compiler
{
    // JSON of the resolved declarations in `bundle`. `user_modules` is the names to include
    // when `include_all` is false - typically the invocation's roots plus `main` for loose
    // sources. empty `user_modules` with `include_all` false dumps nothing
    std::string decls_as_json(
        const AST::Bundle &bundle,
        const std::unordered_set<std::string> &user_modules,
        bool include_all
    );
};

#endif
