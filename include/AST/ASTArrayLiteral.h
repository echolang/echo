#ifndef ASTARRAYLITERAL_H
#define ASTARRAYLITERAL_H

#pragma once

#include "AST/ASTCollectionLiteral.h"
#include "AST/ASTValueType.h"

namespace AST
{
    class ArrayLiteralExprNode;
    class Collector;
    class CoreTypes;
    class ExprNode;

    // **"is the thing on this edge an array literal?"** - null when it is not, and null for a null
    // edge, so the two guards every asker needs are one call. collection_literal_of is the combined
    // peel; this stays the array-only one: the variadic pack, bind_array_literal_to, and
    // collection_entry_type_for
    ArrayLiteralExprNode *array_literal_of(ExprNode *expr);

    // **the sole answer to "what type does a declaration take from its array literal".**
    //
    // `array<int32> $a = [1, 2, 3];` never asks: the declaration said what holds them, and
    // AST::OperatorRewriter fills that. This is the other half - `$a = [1, 2, 3];`, where the elements
    // are all there is, and they are enough.
    //
    // three results and not an optional, for AST::iteration_plan_for's reason: an element may be a call
    // the fixpoint has not settled, and `t_pending` has to be distinguishable from a refusal - otherwise
    // the literal reports "nothing says what this holds" once per round and the first round's guess
    // becomes the diagnostic.
    //
    // reports nothing itself. it has no CodeRef and the caller does - the same split
    // AST::interface_erasure_refusal makes, and for the same reason. the three-result shape is
    // CollectionLiteralLookup; nested entries go through collection_entry_type_for
    typedef CollectionLiteralLookup ArrayLiteralLookup;

    ArrayLiteralLookup array_literal_type_for(
        const ArrayLiteralExprNode &literal, const CoreTypes &core, TypeRegistry &types);

    // **the third way a literal is typed: a destination said so.** answers whether `expr` is an array
    // literal this call typed, and records the type on it if so - the shape AST::bind_null_to has, for
    // the same reason it has it. an argument's destination lives on a declaration nobody has chosen
    // while the expression is being parsed, so AST::CallResolver is the only thing that can say, and
    // it says it here rather than reaching into the node
    //
    // `destination` is the parameter as written, and a **borrow** parameter is answered by the
    // collection it borrows - AST::implicit_conversion_target's peel, for the same reason: the literal
    // builds a value, and the borrow of that value is somebody else's step. what actually reaches the
    // parameter is a place, because AST::OperatorRewriter hoists the built collection into a
    // declaration and puts its name here
    //
    // refuses an undetermined destination rather than recording one: a literal typed `unknown` is
    // indistinguishable from one nothing has typed, and the expansion would then be decided against a
    // type the next round was going to replace
    //
    // **`#[echoc::core: variadic_args]` is the one destination that does not want a collection at all.** the
    // brackets are C's variadic tail there, so the literal is marked as a pack and left otherwise
    // untouched: its elements keep their own types instead of being unified into one `E`, nothing
    // expands it, and this answers *false* - the call is not waiting on anything
    //
    // nested collection literals are bound from collection_fill_of's element, so
    // `array<map<string, int32>> $a = [['a' => 1]]` dest-types the inner map here rather than in expansion
    bool bind_array_literal_to(ExprNode *expr, const ValueType &destination, Collector &collector);
};

#endif
