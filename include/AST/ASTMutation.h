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
    // real copy or can be a `const V&` nobody can tell apart from one. the positive spelling is the safe
    // one, and deliberately: answering false costs a copy, answering true when it is wrong costs the
    // author a ConstViolation on code they were entitled to write.
    //
    // a *proof*, not an analysis. every shape below counts as a write:
    //
    //   $el = v          $el->x = v       $el[$i] = v      $el:$ = v
    //   $el++  $el--     &$el             $el->m()         mv $el
    //   f($el)           return $el       a closure capturing it
    //   an index whose base has already moved into its element_call
    //
    // **`echo` is the one exception**, and it is named through AST::is_print_call rather than
    // special-cased here - it neither writes nor borrows, it is a printf, and without the exception the
    // single most common loop body in the language would never elide.
    //
    // **it must not consult call resolution**, and that is a necessity rather than a simplification: the
    // binding mode decides `$el`'s type, `$el`'s type is what AST::argument_fit scores an overload of
    // `f($el)` on, and the overload is what would tell us whether the parameter is a const borrow. the
    // fixpoint would stall. so every argument counts as a write, and refining that later is monotone -
    // more elision, never less.
    //
    // asked **exactly once**, in the round the loop lowers, on a body still shaped as it was parsed.
    // both halves of that are load-bearing and neither is local: `$el` is untyped until that moment, so
    // OperatorRewriter::resolve_index has deferred every bracket over it and none has moved its base
    // into an element_call yet; and AST::OwnershipPass has not walked the body, because
    // body_is_concrete answers false while an unlowered ForeachNode is in it.
    //
    // the residual risk is a *new* write-shaped node getting RecursiveVisitor's descent and no arm here.
    // that is why the implementation is a RecursiveVisitor subclass and not a hand-rolled switch: a node
    // kind added without a Visitor method does not compile at all
    bool is_never_written(const VarDeclNode &decl, Node &subtree);
};

#endif
