#ifndef ASTPLACEEXPR_H
#define ASTPLACEEXPR_H

#pragma once

#include "AST/ASTNodeTypes.h"
#include "AST/ExprNode.h"

namespace AST
{
    class VarDeclNode;

    // true when the expression denotes storage: it has an address, so `&E`, `E:$` and assigning
    // to E are all meaningful
    //
    // shared deliberately. four places have to agree on this question - the parser rejecting
    // `&($a + $b)`, the adjustment pass deciding value versus place position, the type checker
    // locating a diagnostic, and the lvalue codegen's dispatch. when each kept its own switch
    // they drifted, which is how member reads and member writes ended up disagreeing (todo/A3)
    inline bool is_place_expression(const ExprNode &expr)
    {
        switch (expr.get_node_type()) {
            case NodeType::n_varref:
            case NodeType::n_member_access:
            case NodeType::n_expr_deref:
            case NodeType::n_expr_index:
                return true;

            default:
                return false;
        }
    }

    // true when the expression can stand on the left of an assignment. every place can, plus one
    // shape that is not a place: `E:$` names the pointer slot itself, and writing to it re-seats
    // the pointer (`$p:$ = &$b`). an address (`$p:$:$`, which collapses to `&$p`) has nothing to
    // write back into, so it is not one
    inline bool is_assignable_target(const ExprNode &expr)
    {
        return is_place_expression(expr) || expr.get_node_type() == NodeType::n_expr_peel;
    }

    // the variable an expression ultimately addresses, walking through everything that only
    // re-addresses existing storage rather than naming new storage: every place, plus `&E` and
    // `E:$`, which change what is being said about an address without changing whose it is
    // null when the expression names no variable at all
    //
    // lives next to the predicates above because it encodes the same taxonomy. it used to be a
    // private switch in the type checker, so a new place kind updated the predicate and left the
    // walk behind - and the walk fails silently, by simply not finding the variable
    VarDeclNode *place_root_of(ExprNode *expr);

    // the type an expression yields when it is *read*, which is what an inferred declaration
    // and an assignment target both want
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
