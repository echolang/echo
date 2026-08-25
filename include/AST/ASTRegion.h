#ifndef ASTREGION_H
#define ASTREGION_H

#pragma once

#include <cassert>
#include <utility>
#include <vector>

namespace AST
{
    class Bundle;
    class File;
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class Module;
    class ScopeNode;

    // **how far a region has got**, as a fact of the node rather than a set on a pass.
    //
    // a region is a function body or a file root - the unit AST::OwnershipPass walks exactly once.
    // t_generic is not stored: it is FunctionDeclNode::is_generic(), and nothing runs there.
    //
    //   - **t_open**   types, lowerings, or calls still arriving
    //   - **t_ready**  ownership is walking it. mutation is still allowed; re-entry is not
    //   - **t_owned**  ownership has walked it. minting a drop, a copy, or a hoist into it is an assert
    enum class RegionState
    {
        t_open,
        t_ready,
        t_owned,
    };

    // may this region still be rewritten? the predicate the assert below is, so a test can ask
    // without aborting
    inline bool region_accepts_mutation(RegionState state)
    {
        return state != RegionState::t_owned;
    }

    inline void assert_region_accepts_mutation(RegionState state)
    {
        assert(region_accepts_mutation(state)
            && "a lowering minted into a body ownership has already walked");
    }

    // the region actually being written: a function body if the walk is inside one, else the file root.
    // one spelling, so the five mint sites cannot drift
    void assert_region_accepts_mutation(FunctionDeclNode *fn, File *file);

    // **the deny-list OwnershipPass walked as BodyAnswerable.** true while anything in the scope is
    // still arriving - an unfolder const if, an unlowered foreach, a non-terminal call, an untyped
    // declaration. nested function declarations and type declarations are separate regions and do
    // not count. a new transient node is one arm here
    bool body_is_pending(ScopeNode &scope);

    inline bool body_is_concrete(ScopeNode &scope)
    {
        return !body_is_pending(scope);
    }

    // every call the live tree still contains. skips generic bodies; walks a const if's condition
    // so it can fold, and only the taken arm once it has. a different visitor from body_is_pending:
    // that one stops at the first unfinished node, this one has to see every live call
    std::vector<std::pair<FunctionCallExprNode *, Module *>> live_calls(Bundle &bundle);
};

#endif
