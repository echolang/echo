#ifndef ASTMAPLITERAL_H
#define ASTMAPLITERAL_H

#pragma once

#include "AST/ASTCollectionLiteral.h"
#include "AST/ASTValueType.h"

namespace AST
{
    class Collector;
    class CoreTypes;
    class ExprNode;
    class MapLiteralExprNode;

    // **"is the thing on this edge a map literal?"** - null when it is not, and null for a null
    // edge, so the two guards every asker needs are one call. the twin of AST::array_literal_of
    MapLiteralExprNode *map_literal_of(ExprNode *expr);

    // **the sole answer to "what type does a declaration take from its map literal".**
    //
    // `map<string, int32> $m = ["a" => 1];` never asks: the declaration said what holds them.
    // this is the other half - `$m = ["a" => 1];`, where the first entry's key and value are all
    // there is, and they are enough. mints `map<K, V>` through `#[echoc::core: map]`. an
    // `ordered_map` destination never reaches here: it already named the type
    //
    // three results, for AST::array_literal_type_for's reason. the shape is
    // CollectionLiteralLookup; nested entries go through collection_entry_type_for
    typedef CollectionLiteralLookup MapLiteralLookup;

    MapLiteralLookup map_literal_type_for(
        const MapLiteralExprNode &literal, const CoreTypes &core, TypeRegistry &types);

    // **the third way a literal is typed: a destination said so.** answers whether `expr` is a
    // map literal this call typed. a borrow parameter is answered by the collection it borrows
    // (AST::implicit_conversion_target). refuses an undetermined destination rather than
    // recording one, for bind_array_literal_to's reason. nested collection literals are bound
    // from collection_fill_of's key and value
    bool bind_map_literal_to(ExprNode *expr, const ValueType &destination, Collector &collector);
};

#endif
