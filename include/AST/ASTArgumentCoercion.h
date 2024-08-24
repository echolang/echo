#ifndef ASTARGUMENTCOERCION_H
#define ASTARGUMENTCOERCION_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "AST/VarRefNode.h"

namespace AST
{
    // when a callee parameter is a pointer and the argument is an addressable lvalue
    // variable, wrap it in a VarPtrExprNode so its address (the alloca) is passed instead
    // of a loaded value. this is the implicit form of the address-of that `&$x` makes
    // explicit; performing it here (in the pre-codegen coercion pass) keeps codegen from
    // having to discriminate raw argument node kinds. returns the possibly-wrapped argument.
    // idempotent: an already-wrapped VarPtrExprNode is not a VarRefNode and is left as-is
    inline ExprNode *coerce_arg_to_pointer_param(NodeCollection &nodes, ExprNode *arg, const ValueType &expected)
    {
        if (!expected.is_pointer()) {
            return arg;
        }

        auto ref = make_ref(arg);
        if (!ref.has_type<VarRefNode>()) {
            return arg;
        }

        auto *varref = ref.get_ptr<VarRefNode>();
        if (!varref->is_var()) {
            return arg;
        }

        return &nodes.emplace_back<VarPtrExprNode>(varref);
    }
};

#endif
