#ifndef ASTLASTREAD_H
#define ASTLASTREAD_H

#pragma once

#include <unordered_set>
#include <vector>

namespace AST
{
    class ExprNode;
    class ScopeNode;
    class VarDeclNode;

    // reads of a body's owners that hand the value over rather than copy. `return f($x)` moves `$x`
    // the same way `return $x` does. OwnershipPass asks once per body. a proof: a read is in the
    // set only when all of these hold
    //
    //   - it sits in a `return`, or it is a collection-literal hoist (`statement_temp`)
    //     whose last mention is this read (the temp dies with the statement)
    //   - last mention in source order
    //   - only mention in its statement (`return $out->fold($out)` stays a copy)
    //   - no loop / rejoining branch that does not also enclose the declaration
    //   - a `return match` only when every completing arm consumes it (the arm value,
    //     or a by-value argument): mixed take/read copies
    //   - no closure captures it
    std::unordered_set<const ExprNode *> handover_reads_in(
        ScopeNode &body,
        const std::vector<VarDeclNode *> &parameters
    );
};

#endif
