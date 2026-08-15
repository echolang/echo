#ifndef ASTFOREACHLOWERING_H
#define ASTFOREACHLOWERING_H

#pragma once

#include "AST/ASTCollector.h"
#include "AST/ASTFixpointLowering.h"
#include "AST/ASTIteration.h"
#include "AST/ASTValueType.h"

#include "Token.h"

#include <string>

namespace AST
{
    class Bundle;
    class ExprNode;
    class ForeachNode;
    class FunctionCallExprNode;
    class ScopeNode;
    class VarDeclNode;

    // rewrites every `foreach` into the iterator declaration and the `while` a hand-written loop would
    // have been:
    //
    //     {                                   // a scope of its own, so $__it dies at loop exit
    //         $__it = <source>->iterate();
    //         while ($__it->advance()) {
    //             K  $k  = $__it->key();      // keyed form only
    //             V& $el = $__it->current();  // or a copy, per the binding mode
    //             <the user's body>
    //         }
    //     }
    //
    // **inside AST::Monomorphizer's fixpoint**, between the operator rewriter and the re-derivation
    // sweep. after the rewriter because `foreach ($grid[0] as $row)`'s source has no type until the
    // bracket has become an `operator []` call; before the re-derivation because `$__it` is declared
    // untyped and that sweep is what types it; before the ownership pass because a body is walked
    // exactly once, ever, and it must not be walked while a foreach is still in it.
    //
    // its own pass rather than one more rule in AST::OperatorRewriter: that class's header states it is
    // one walk over rules sharing a predicate and an operand normalizer, and a foreach shares neither.
    // the cost is a fifth tree walk per round, which belongs in the fixpoint's own budget rather than being paid
    // for with a false claim in a header.
    //
    // the tree is walked through scope children and **never** through nodes.of_type<ForeachNode>():
    // NodeCollection owns a detached node forever, so an of_type sweep would re-process one that has
    // already been lowered away
    //
    // **built on AST::RecursiveVisitor rather than on a switch of its own** - through
    // AST::FixpointLowering, which is the chassis every pass in the fixpoint shares. a hand-rolled
    // statement walk is a second answer to "what are this node's owned children", and the way it fails
    // is by omission: a missing arm silently never reaches the loops under it, and a `foreach` that is
    // never lowered surfaces as the InternalCompilerException the monomorphizer throws for a survivor.
    // the visitor is total by construction - a node kind added without a visit method does not compile -
    // so only the one edge this pass treats differently is overridden below
    class ForeachLowering : private FixpointLowering
    {
    public:
        ForeachLowering(Bundle &bundle);

        // the chassis' own two entry points, and the whole of this pass's public surface.
        //
        // `finalize` is exposed because a loop *has* a pending state, and without an exit for it
        // `pending` had none at all: a source that never becomes typed - `$a = [];`, then a loop over
        // `$a` - left the node in the tree, and PointerAdjuster's throw for a survivor aborted the
        // process on top of a perfectly good diagnostic nobody had printed yet
        using FixpointLowering::run_round;
        using FixpointLowering::finalize;

    private:
        // indexed rather than the base's ranged walk, because lower() replaces the child in place and
        // the wrapper it leaves behind is descended into on the same index - which is what lets a
        // nested foreach lower in the same round as the one around it
        void visitScope(ScopeNode &node) override;

        // lowers `scope.children[index]`, which is a ForeachNode. leaves it in place when the source's
        // type is not settled yet, and replaces it with an empty scope when it refused - a refused node
        // must not survive, because run_semantic_passes runs PointerAdjuster *before* it gates on
        // has_critical_issues(), and a survivor turns a good located diagnostic into an unlocated
        // internal error stacked on top of it
        void lower(ScopeNode &scope, size_t index);

        // reports and discards, in that order - one function, because forgetting the discard is the
        // silent half and there are three arms that must not forget it. the kind is the thing that
        // went wrong, not a place in this pass
        template <typename Issue>
        void refuse(
            ScopeNode &scope, size_t index, ForeachNode &loop, const TokenReference &at, std::string why);

        void discard(ScopeNode &scope, size_t index, ForeachNode &loop);

        // `$__it->name()`, left unresolved for the fixpoint's own settle_calls to finish. a fresh
        // receiver subtree per call: PointerAdjuster rewrites edges in place, so a shared one would
        // collect a deref per use
        FunctionCallExprNode &iterator_call(
            VarDeclNode &iterator,
            const std::string &name,
            const TokenReference &at
        );
    };
};

template <typename Issue>
void AST::ForeachLowering::refuse(
    ScopeNode &scope, size_t index, ForeachNode &loop, const TokenReference &at, std::string why)
{
    _collector.collect_issue<Issue>(code_ref_for(at), std::move(why));
    discard(scope, index, loop);
}

#endif
