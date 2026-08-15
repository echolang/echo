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

    // **which reads of a body's own owners hand the value over rather than copy it**, as the set of the
    // reading nodes themselves.
    //
    // A `return` already moves the local it hands back, and no `mv` is written because there is nothing
    // else a returned local could be (AST::ValueDestination::t_return). This is that same rule read one
    // step further out: **`return f($x)` hands `$x` over too.** The statement is the end of `$x`'s life
    // whichever of the two shapes the author wrote, so which one they chose is not something the value
    // should be able to tell - and for an owning type the difference is a whole allocation, which is
    // what `return .ok($out)` was paying twice per call.
    //
    // AST::OwnershipPass asks it once per body and looks each arriving by-value argument up.
    //
    // **a proof, not an analysis**, on AST::is_never_written's terms: the positive answer is the one that
    // costs correctness, so every shape this cannot see through is simply absent from the set. A read is
    // in it only when all six hold, and each is a way the pass would otherwise be wrong rather than
    // merely tight:
    //
    //   - **it sits in a `return`.** the narrow gate, and the one that keeps this an optimisation rather
    //     than a change to when values die: everywhere else a by-value argument is a copy, the caller
    //     keeps its reference and the object outlives the call - which is a rule the language states, and
    //     `tests_eco/classes/refcount_transfer` pins. Widening this to every provable last read is a
    //     language decision and not a compiler one: it brings every destructor and every `weak<T>`
    //     forward to the last *use* of a value rather than the end of its scope
    //   - it is the **last mention in source order**. any later mention is a read of a value that has
    //     been handed away - and source order over-approximates execution order in the one direction
    //     that is safe, since the only way an earlier mention runs later is a loop, which is the next rule
    //   - it is the **only mention in its statement**, which is what keeps `return $out->fold($out)` a
    //     copy. the receiver borrows the very storage the argument would hand over, and a callee that
    //     drops its parameter then frees what its own `$this` still names
    //   - **no loop encloses it that does not also enclose the declaration.** a move that runs twice
    //     reads a value that is no longer there the second time, which is the diagnostic
    //     AST::OwnershipPass::walk_loop already raises for an explicit `mv`
    //   - **no branch that rejoins encloses it that does not also enclose the declaration.** a value
    //     moved on one arm of an `if` is one nothing destroys on the other, and that is
    //     report_conditional_move's refusal. an arm that always leaves is not one of these: it does not
    //     reach the join, so its moves go with it, which is what keeps `if (...) { return .ok($out); }`
    //     in. a `match` arm is the same question: one that always leaves does not rejoin. a match
    //     used as a return value still joins its arms at a phi, so handing over on only one of
    //     them is the conditional move OwnershipPass reports
    //   - **no closure captures the declaration.** the capture is read whenever the callable is, which is
    //     a point no walk of this body can see
    //
    // `parameters` are the body's own, which are owners exactly as its locals are - a by-value parameter
    // of an owning type is destroyed at the end of the body - and are the one set of declarations a walk
    // of the body does not reach. Everything else a read could name is either declared in here or is not
    // this body's to hand over
    std::unordered_set<const ExprNode *> handover_reads_in(
        ScopeNode &body,
        const std::vector<VarDeclNode *> &parameters
    );
};

#endif
