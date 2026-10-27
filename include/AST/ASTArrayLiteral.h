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

    // dest-type an array literal from a parameter. a borrow peels to the collection. refuses unknown.
    // `variadic_args` is a pack, not a collection: answers false, elements keep their own types.
    // true when this call wrote a bound_type (or a nested one)
    bool bind_array_literal_to(ExprNode *expr, const ValueType &destination, Collector &collector);
};

#endif
