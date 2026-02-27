#ifndef ASTMUTATION_H
#define ASTMUTATION_H

#pragma once

namespace AST
{
    class Node;
    class VarDeclNode;

    // **can nothing in `subtree` write through `decl`?**
    //
    // asked by AST::ForeachLowering, to decide whether `foreach ($a as $el)`'s by-value binding needs a
    // real copy or can be a `const V&` nobody can tell apart from one.
    //
    // The positive spelling is the safe one, and that is deliberate. Answering false costs a copy.
    // Answering true when it is wrong costs the author a ConstViolation on code they were entitled to
    // write.
    //
    // a *proof*, not an analysis. every shape below counts as a write:
    //
    //   $el = v          $el->x = v       $el[$i] = v      $el:$ = v
    //   $el++  $el--     &$el             $el->m()         mv $el
    //   f($el)           return $el       a closure capturing it
    //   an index whose base has already moved into its element_call
    //
    // **`echo` is the one exception.** it neither writes nor borrows - it is a printf - and without the
    // exception the single most common loop body in the language would never elide. Named through
    // AST::is_print_call rather than special-cased here.
    //
    // **it must not consult call resolution**, and that is a necessity rather than a simplification.
    // Follow the loop: the binding mode decides `$el`'s type, `$el`'s type is what AST::argument_fit
    // scores an overload of `f($el)` on, and the overload is what would tell us whether the parameter
    // is a const borrow. The fixpoint would stall.
    //
    // So every argument counts as a write. Refining that later is monotone - more elision, never less.
    //
    // asked **exactly once**, in the round the loop lowers, on a body still shaped as it was parsed.
    // Both halves of that are load-bearing, and neither is a local fact:
    //
    //   - `$el` is untyped until that moment, so OperatorRewriter::resolve_index has deferred every
    //     bracket over it and none has moved its base into an element_call yet
    //   - AST::OwnershipPass has not walked the body, because body_is_concrete answers false while an
    //     unlowered ForeachNode is in it
    //
    // The residual risk is a *new* write-shaped node getting RecursiveVisitor's descent and no arm
    // here. That is why the implementation is a RecursiveVisitor subclass rather than a hand-rolled
    // switch: a node kind added without a Visitor method does not compile at all
    bool is_never_written(const VarDeclNode &decl, Node &subtree);
};

#endif
