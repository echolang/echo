#ifndef ASTMANGLER_H
#define ASTMANGLER_H

#pragma once

#include <string>

namespace AST
{
    class FunctionDeclNode;
    class FunctionCallExprNode;

    // mangled name is just a string 
    typedef std::string mangled_id_t;

    mangled_id_t mangle_function_name(const FunctionDeclNode *func_decl);
    mangled_id_t mangle_function_name(const FunctionCallExprNode *func_call);
}

#endif