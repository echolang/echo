#ifndef ASTARGUMENTCOERCION_H
#define ASTARGUMENTCOERCION_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"

namespace AST
{
    // when a callee parameter is a borrow and the argument is addressable, wrap it in an
    // AddrOfExprNode so its address is passed instead of a loaded value. this is the implicit
    // form of the address-of that `&$x` makes explicit; performing it here (in the pre-codegen
    // coercion pass) keeps codegen from having to discriminate raw argument node kinds.
    // returns the possibly-wrapped argument.
    inline ExprNode *coerce_arg_to_pointer_param(NodeCollection &nodes, ExprNode *arg, const ValueType &expected)
    {
        // only a borrow parameter (`T&`) auto-borrows. a nullable `ptr<T>` parameter does not:
        // taking an address is a decision the caller should be able to see in the source
        // (book/concept/pointers_and_refs_v2.md, "Passing to functions")
        if (!expected.is_pointer() || expected.is_nullable()) {
            return arg;
        }

        // an argument that already fits is left alone. this used to be implicit in the pointer
        // flag being idempotent; now wrapping a ptr<int32> for a ptr<int32> parameter would
        // build a ptr<ptr<int32>>
        if (is_implicitly_convertible(arg->result_type(), expected)) {
            return arg;
        }

        // any place will do, not just a bare variable: `f($s->field)` borrows the field.
        // an argument with no storage (a literal, a call result) is left alone and reported
        // by the type checker rather than silently having an address invented for it
        if (!is_place_expression(*arg)) {
            return arg;
        }

        return &nodes.emplace_back<AddrOfExprNode>(arg);
    }
};

#endif
