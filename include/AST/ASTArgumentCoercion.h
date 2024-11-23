#ifndef ASTARGUMENTCOERCION_H
#define ASTARGUMENTCOERCION_H

#pragma once

#include "AST/ASTArgumentFit.h"
#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"

namespace AST
{
    // when a callee parameter is a borrow and the argument is addressable, wrap it in an
    // AddrOfExprNode so its address is passed instead of a loaded value. this is the implicit
    // form of the address-of that `&$x` makes explicit; performing it here (in the pre-codegen
    // coercion pass) keeps codegen from having to discriminate raw argument node kinds
    // returns the possibly-wrapped argument
    inline ExprNode *coerce_arg_to_pointer_param(NodeCollection &nodes, ExprNode *arg, const ValueType &expected)
    {
        // the whole rule - which parameters auto-borrow, which arguments can be borrowed, and
        // that an argument which already fits is left alone - lives in argument_fit, because
        // overload resolution has to predict this decision exactly. a candidate accepted there
        // and then not wrapped here would reach codegen passing a value where an address is
        // expected
        if (argument_fit(arg->result_type(), arg, expected) != ArgumentFit::t_borrow) {
            return arg;
        }

        return &nodes.emplace_back<AddrOfExprNode>(arg);
    }
};

#endif
