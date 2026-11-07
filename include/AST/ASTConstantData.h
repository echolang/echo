#ifndef ASTCONSTANTDATA_H
#define ASTCONSTANTDATA_H

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace AST
{
    class ArrayLiteralExprNode;
    class NodeCollection;
    class ValueType;

    // **can this array literal fold to interned integer bits?** dest-types each element, then
    // const_fold. empty when an element refused, did not fold, or is not that integer.
    // writes typed literals back onto the array so a later clone sees them.
    // CollectionLiteralExpansion and ConstantExpander both mint ConstantData from this
    std::optional<std::vector<uint64_t>> fold_integer_table(
        ArrayLiteralExprNode &array,
        const ValueType &element,
        NodeCollection &nodes);
};

#endif
