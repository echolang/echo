#ifndef ASTMUTATION_H
#define ASTMUTATION_H

#pragma once

namespace AST
{
    class Collector;
    class Node;
    class VarDeclNode;

    // **can nothing in `subtree` write through `decl`, and would eliding it to `const V&` leave
    // every call indistinguishable?**
    //
    // asked by AST::ForeachLowering, to decide whether `foreach ($a as $el)`'s by-value binding needs a
    // real copy or can be a `const V&` nobody can tell apart from one. MatchResolution asks the same
    // proof of a place subject: a wrong true seats the original without a const copy, so an arm can
    // write it. a wrong false costs a copy. foreach's wrong true is ConstViolation on legal code.
    //
    // a *proof*, not an analysis. it does not write the tree. ForeachLowering seats V and types every
    // `?->` marker through `$el` before asking, so `$el?->trace()` can see the set. every shape below
    // counts as a write:
    //
    //   $el = v          $el->x = v       $el[$i] = v      $el:$ = v
    //   $el++  $el--     &$el as a value  a mutable method / T& / mv parameter
    //   return $el       a closure capturing it
    //   an index whose base has already moved into its element_call
    //
    // **`echo` is the one exception.** it neither writes nor borrows - it is a printf - and without the
    // exception the single most common loop body in the language would never elide. Named through
    // AST::is_print_call rather than special-cased here.
    //
    // **it must not score the call against `$el`.** Follow the loop: the binding mode decides `$el`'s
    // type, `$el`'s type is what AST::argument_fit scores an overload of `f($el)` on, and the winner
    // is what would tell us whether the parameter is a const borrow. Asking argument_fit would stall.
    //
    // The overload *set* is a different question, and CallResolver::candidates_for is its owner.
    // AST::bind_arguments fills the slot, so a labeled `$item: $el` sees the parameter that holds it.
    // An argument of `$el` prevents elision when any bound candidate's slot writes (`T&`, exclusive
    // access, `mv`, or a by-value copy that cannot read a const source), when a slot is not V itself
    // (a promotion `usize` → `uint64` at `str::from`, which `const usize&` cannot compose), or when a
    // by-value slot sits next to a borrow slot in the same set. `f(T)` beside `f(const T&)` would flip
    // the winner: by-value `$el` ranks the copy exact, and eliding makes the const borrow exact. If
    // every remaining candidate only reads V, `look(const Item& $item)` and `$item->trace()` elide.
    // A mixed `look(Item&)` / `look(const Item&)` set still copies.
    //
    // asked **exactly once**, in the round the loop lowers, on a body still shaped as it was parsed.
    // Both halves of that are load-bearing, and neither is a local fact:
    //
    //   - OperatorRewriter::resolve_index has deferred every bracket over `$el` (it was untyped
    //     until ForeachLowering seated V for this walk) and none has moved its base into an
    //     element_call yet
    //   - AST::OwnershipPass has not walked the body, because body_is_concrete answers false while an
    //     unlowered ForeachNode is in it
    //
    // The residual risk is a *new* write-shaped node getting RecursiveVisitor's descent and no arm
    // here. That is why the implementation is a RecursiveVisitor subclass rather than a hand-rolled
    // switch: a node kind added without a Visitor method does not compile at all
    bool is_never_written(const VarDeclNode &decl, Node &subtree, Collector &collector);
};

#endif
