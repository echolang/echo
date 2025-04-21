#ifndef ASTPLACEEXPR_H
#define ASTPLACEEXPR_H

#pragma once

#include "AST/ASTNodeTypes.h"
#include "AST/ExprNode.h"

#include <optional>

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

            // **a `?->` chain's unwrapped base is a place**, and it has to be: everything after the `?->`
            // is an ordinary member chain, and a method call in there needs a receiver with an address.
            // the chain materialises the unwrapped value into a slot before running the continuation, so
            // there genuinely is one - see ExprCodegen::gen_optional_chain
            //
            // it owns nothing, exactly like `$this`: the slot borrows the value the chain already holds,
            // so nothing here is retained and nothing is dropped
            case NodeType::n_expr_chain_base:
                return true;

            default:
                return false;
        }
    }

    // **can this expression be given storage?** a value nobody stored has no address, but it can be
    // bound to a temporary and then addressed - which is exactly what a method receiver needs, since a
    // method takes the address of the value it is called on (todo/A13b), and what lets a literal
    // answer a borrow parameter (`inc(41)` against `inc(int32& $x)`, todo/A13c)
    //
    // a call is the shape that matters most: it is where a value with no home usually comes from. but
    // every one of these is the same thing - a value the program computed and did not name - so the
    // list is deliberately about *shape*, not about where the value came from
    //
    // **an allow-list, not "everything that is not a place"**, and each exclusion is load-bearing:
    //
    //  - `n_expr_array_literal` has its own rule ("an array literal fills storage, so it has to name
    //    it"), and leaving it out is what keeps that diagnostic by construction rather than by luck;
    //  - `n_null` carries no type of its own - NullNode::bound_type is set by the destination, so
    //    there is nothing here to size a slot from;
    //  - `n_expr_addrof` and `n_expr_peel` are already values that *mean* an address; giving one
    //    storage hands out a ptr<ptr<T>>, which AST::OwnershipPass refuses anyway;
    //  - `n_expr_temp_bind` already carries its request outward through the pending queue.
    //
    // asked in the parser, which is where a receiver is decided and no type is known yet, and in
    // AST::argument_fit, which has the expression but not yet a decision. AST::OwnershipPass asks the
    // type question that goes with it and is what actually binds the temporary
    inline bool can_bind_temporary(const ExprNode &expr)
    {
        switch (expr.get_node_type()) {
            case NodeType::n_expr_call:
            case NodeType::n_expr_indirect_call:
            case NodeType::n_literal:
            case NodeType::n_literal_float:
            case NodeType::n_literal_int:
            case NodeType::n_literal_bool:
            case NodeType::n_literal_string:
            case NodeType::n_expr_binary:
            case NodeType::n_expr_unary:
            case NodeType::n_type_cast:
                return true;

            default:
                return false;
        }
    }

    // **is there anything here to mint storage for at all?** the half the two requesting arms - a member
    // access's base and an `&`'s operand - genuinely share, so `$o->get()->tag` and `$o->get()->size()`
    // cannot answer it differently.
    //
    // the place test is first because it is the cheap one and it is what almost every operand answers:
    // every compiler-inserted borrow - a receiver, a drop, a CallResolver coercion - is an address of a
    // place, and a member base is a place in all but the shape this exists for. deriving the type ahead of
    // it would re-walk a whole `->` chain per link, once per fixpoint round, and throw the answer away
    //
    // a **pointer** operand needs nothing: a borrow-returning call already is the address (todo/A13a)
    //
    // answers *with the type it had to derive* rather than with a bool, so the two narrower questions
    // below share the one derivation. MemberAccessNode::result_type() recurses the whole `->` chain, so a
    // second call is quadratic in chain depth on a pass that runs once per fixpoint round
    inline std::optional<ValueType> storageless_operand_type(const ExprNode &operand)
    {
        if (is_place_expression(operand)) {
            return std::nullopt;
        }

        const ValueType type = operand.result_type();

        if (type.is_pointer()) {
            return std::nullopt;
        }

        return type;
    }

    // **does an `&`'s operand need storage minted for it?** everything that lacks storage and has a type
    // concrete enough to size a slot from - which after todo/A13c is every value the language can
    // produce, a literal and an arithmetic result included, not only the struct and class a member base
    // has to be
    //
    // is_undetermined_type is the one predicate covering all three shapes that cannot be given a slot,
    // and each for its own reason: an unsettled call, whose result_type() is still void; an unknown; and
    // anything still mentioning a type parameter, which a template body must never allocate for before
    // the monomorphizer has substituted it. all three are left to the type checker's located error,
    // which is where a reason belongs
    //
    // **two passes must agree on this exactly**, which is why it lives here rather than beside either:
    // AST::OwnershipPass mints the slot, and AST::TypeChecker's guard rail reports the case where nothing
    // did. a guard rail spelling its own copy of the predicate it polices cannot catch the predicate
    // drifting, which is the one thing it is for
    inline bool borrow_operand_needs_storage(const ExprNode &operand)
    {
        const std::optional<ValueType> type = storageless_operand_type(operand);
        return type.has_value() && !is_undetermined_type(*type);
    }

    // **does a member access's base need storage minted for it?** a strictly narrower question than the
    // one above, and narrower on purpose: this arm does not ask whether the value *can* be stored but
    // whether there is a member to reach once it is. so it stays on has_property_layout, which leaves an
    // interface base - requirements, storing nothing a `->` can index - on the type checker's error
    // rather than silently binding a temporary nothing will read
    inline bool member_base_needs_storage(const ExprNode &operand)
    {
        const std::optional<ValueType> type = storageless_operand_type(operand);
        return type.has_value() && type->has_property_layout();
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

    // **where does a diagnostic about this expression point?** every shape carries a token somewhere, so
    // this walks to whichever is nearest the surface rather than falling back to the file's first token -
    // a diagnostic at line 1 is worse than none
    //
    // beside place_root_of because it encodes the same taxonomy over the same node set, and shared for the
    // reason the predicates above it are: two passes now report about an expression that names no variable.
    // AST::OwnershipPass, about a temporary it bound or refused, and AST::TypeChecker, about a borrow
    // whose operand nothing gave storage to. a second copy would answer the same question differently the
    // first time either grew an arm
    //
    // the shapes that carry no token of their own - `&`, a deref, an implicit cast, none of which the
    // author ever wrote - borrow their operand's
    const TokenReference &location_of_expression(ExprNode *expr);

    // the type an expression yields when it is *read*, which is what an inferred declaration
    // and an assignment target both want
    //
    // reading a place that holds a pointer auto-dereferences it once, so `$copy = $r` over an
    // `int32&` infers int32 and copies the value. an expression that is not a place is already
    // the value it means - `&$x` yields an address, so `$ref = &$var` still infers a pointer
    // the overload taking a `result_type()` the caller already has, for a site that wants both the
    // read type and the raw one - result_type() walks the expression's subtree, so a caller holding
    // the answer passes it rather than provoking it again. the place rule stays in one place
    inline ValueType value_result_type(const ExprNode &expr, const ValueType &result_type)
    {
        return is_place_expression(expr) ? value_type_of(result_type) : result_type;
    }

    inline ValueType value_result_type(const ExprNode &expr)
    {
        return value_result_type(expr, expr.result_type());
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
    //
    // the overload below is the second half on its own, for the **third** moment: an array literal's
    // `result_type()` is unknown by construction, so a declaration initialized by one is typed from
    // its *elements* by AST::array_literal_type_for, and only the `const` half applies there
    inline ValueType infer_declaration_type(const ValueType &inferred, bool is_const)
    {
        return is_const ? ValueType::make_const(inferred) : inferred;
    }

    inline ValueType infer_declaration_type(const ExprNode &init_expr, bool is_const)
    {
        return infer_declaration_type(value_result_type(init_expr), is_const);
    }
};

#endif
