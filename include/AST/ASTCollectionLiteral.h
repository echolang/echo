#ifndef ASTCOLLECTIONLITERAL_H
#define ASTCOLLECTIONLITERAL_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    class ArrayLiteralExprNode;
    class CollectionLiteralExprNode;
    class Collector;
    class CoreTypes;
    class ExprNode;
    class MapLiteralExprNode;

    // **"is the thing on this edge a collection literal?"** array first, then map. null when it is
    // not, and when the edge is null, so the two guards every asker needs are one call.
    //
    // array first is load-bearing: `[...]` at `variadic_args` is an array node, and
    // AST::bind_array_literal_to is the one owner of that pack. bind_collection_literal_to
    // keeps the same order
    CollectionLiteralExprNode *collection_literal_of(ExprNode *expr);

    // three results, for AST::iteration_plan_for's reason: an entry may be a call the fixpoint
    // has not settled, and `t_pending` has to be distinguishable from a refusal
    struct CollectionLiteralLookup
    {
        enum class Result
        {
            t_ok,

            // ask again next round: an entry's type is not settled yet
            t_pending,

            // `refusal` is a whole sentence, phrased for the author of the literal
            t_refused,
        };

        Result result = Result::t_pending;

        // the applied collection type, with no opinion about `const` - the declaration's own,
        // which the caller puts back on top exactly as AST::infer_declaration_type does
        ValueType type;

        std::string refusal;
    };

    // **how this destination fills**, from the write contract (append / keyed `[]=`) or `T[N]`'s
    // element, never from template-argument positions. one answer for nested bind and for expansion
    struct CollectionFill
    {
        enum class Kind
        {
            t_none,
            t_inline,
            t_append,
            t_index,
            t_keyed,
        };

        Kind kind = Kind::t_none;

        // t_inline / t_append / t_index
        ValueType element;

        // t_keyed
        ValueType key;
        ValueType value;

        // t_inline / t_index
        std::optional<uint64_t> length;
    };

    CollectionFill collection_fill_of(const ValueType &destination, Collector &collector);

    // **the dest-typed bind, array first.** answers whether `expr` is a collection literal this
    // call typed. a pack at `variadic_args` is bind_array_literal_to's and this answers false.
    // recurses into nested collection literals from collection_fill_of
    bool bind_collection_literal_to(ExprNode *expr, const ValueType &destination, Collector &collector);

    // **what type does this nested entry have**, for the two `*_type_for` owners. array, then
    // map, then `value_result_type`. a null entry is pending
    CollectionLiteralLookup collection_entry_type_for(ExprNode *entry, const CoreTypes &core, TypeRegistry &types);

    // dispatch onto the two type owners. expansion is the one caller that already knows it has
    // a collection literal and still has to ask which
    CollectionLiteralLookup collection_literal_type_for(
        CollectionLiteralExprNode &literal,
        const CoreTypes &core,
        TypeRegistry &types
    );
};

#endif
