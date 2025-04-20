#ifndef ASTCONTROLFLOW_H
#define ASTCONTROLFLOW_H

#pragma once

namespace AST
{
    class ScopeNode;

    // **does control always leave this scope?** true when the point past its closing brace cannot be
    // reached - because some statement in it returns, dies, or branches with every arm doing one of those.
    //
    // *some* statement, not the last one: everything written after a statement that leaves is unreachable
    // too, so a `return` with dead code behind it still answers true.
    //
    // two callers, which is why this has a header of its own rather than living beside `guard` in
    // ASTNullability.h where it started:
    //
    // - `guard`'s else block, which has to leave. a guard binds a name that is only meaningful on the path
    //   where the value was there, so an else arm that ran on and rejoined would leave that name bound to
    //   nothing. refusing at the declaration is what makes the binding's promise true by construction
    //   rather than by the author remembering
    //
    // - AST::OwnershipPass, deciding whether a scope owes its locals a teardown *after* its last statement.
    //   when control never gets there, the drops a `return` already collected onto ReturnNode::unwind are
    //   the whole story and a second set is dead tree - one that is still type-checked, and that mints a
    //   generic call site for a drop of a `Box<int32>` local
    //
    // the two are the same question and were not asked the same way: the ownership pass used to test its
    // scope's last child for `ReturnNode` and nothing else, so a `die` tail, an `if` whose arms both
    // return, and any dead statement written after a `return` each duplicated the drop set
    //
    // deliberately structural rather than a reachability analysis - the *shape* of each statement, and for
    // an `if` the shape of its arms, recursively. this language has no `break` or `continue` yet for a real
    // analysis to be complete over, and codegen's own rule is the same shape: StmtCodegen::gen_scope stops
    // at the first terminated block. **it must only ever answer true when it is certain**, since a false
    // positive here is a leak in one caller and a broken binding in the other
    bool scope_always_exits(const ScopeNode &scope);
};

#endif
