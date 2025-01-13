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

    // **the** type an inferred declaration takes from its initializer: the value the initializer
    // reads as, with the `const` the declaration was written with put back on top - the inferred type
    // is the single source of truth, const included, so there is no node-level flag to disagree with
    //
    // two askers at two moments, which is the only difference between them: the parser, where the
    // declaration is written, and the monomorphizer's re-derivation sweep, for a declaration whose
    // initializer was a call that could not be settled yet. they used to spell it out separately, and
    // the sweep's spelling dropped both halves - so `const $x = f();` silently lost its `const`, and
    // lost it only for the initializers the fixpoint had to finish
    inline ValueType infer_declaration_type(const ExprNode &init_expr, bool is_const)
    {
        const ValueType inferred = value_result_type(init_expr);
        return is_const ? ValueType::make_const(inferred) : inferred;
    }
};

#endif
