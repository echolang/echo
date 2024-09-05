#ifndef ASTPLACEEXPR_H
#define ASTPLACEEXPR_H

#pragma once

#include "AST/ASTNodeTypes.h"
#include "AST/ExprNode.h"

namespace AST
{
    // true when the expression denotes storage: it has an address, so `&E`, `E:$` and assigning
    // to E are all meaningful.
    //
    // shared deliberately. four places have to agree on this question - the parser rejecting
    // `&($a + $b)`, the adjustment pass deciding value versus place position, the type checker
    // locating a diagnostic, and the lvalue codegen's dispatch. when each kept its own switch
    // they drifted, which is how member reads and member writes ended up disagreeing (todo/A3)
    inline bool is_place_expression(const ExprNode &expr)
    {
        switch (expr.get_node_type())
        {
            case NodeType::n_varref:
            case NodeType::n_member_access:
            case NodeType::n_expr_deref:
            case NodeType::n_expr_index:
                return true;

            default:
                return false;
        }
    }

    // the type an expression yields when it is *read*, which is what an inferred declaration
    // and an assignment target both want.
    //
    // reading a place that holds a pointer auto-dereferences it once, so `$copy = $r` over an
    // `int32&` infers int32 and copies the value. an expression that is not a place is already
    // the value it means - `&$x` yields an address, so `$ref = &$var` still infers a pointer
    inline ValueType value_result_type(const ExprNode &expr)
    {
        ValueType type = expr.result_type();
        return is_place_expression(expr) ? value_type_of(type) : type;
    }
};

#endif
