#ifndef ASTFOREACHLOWERING_H
#define ASTFOREACHLOWERING_H

#pragma once

#include "AST/ASTIteration.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTValueType.h"

#include "Token.h"

#include <string>

namespace AST
{
    class Bundle;
    class Collector;
    struct CodeRef;
    class ExprNode;
    class File;
    class ForeachNode;
    class FunctionCallExprNode;
    class Module;
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
    // the cost is a fifth tree walk per round, which belongs in todo/X3's ledger rather than being paid
    // for with a false claim in a header.
    //
    // the tree is walked through scope children and **never** through nodes.of_type<ForeachNode>():
    // NodeCollection owns a detached node forever, so an of_type sweep would re-process one that has
    // already been lowered away
    //
    // **built on AST::RecursiveVisitor rather than on a switch of its own.** a hand-rolled statement
    // walk is a second answer to "what are this node's owned children", and the way it fails is by
    // omission: a missing arm silently never reaches the loops under it, and a `foreach` that is never
    // lowered surfaces as the InternalCompilerException the monomorphizer throws for a survivor. the
    // visitor is total by construction - a node kind added without a visit method does not compile -
    // so only the two edges this pass treats differently are overridden below
    class ForeachLowering : private RecursiveVisitor
    {
    public:
        ForeachLowering(Bundle &bundle);

        // answers whether anything changed, so the fixpoint can report progress
        bool run_round();

        // **the fixpoint's exit obligation**: one last round in which "the source is not settled yet"
        // is a refusal. being out of rounds is the proof that nothing was ever going to settle it,
        // which is Monomorphizer::finalize_calls' reasoning and its moment.
        //
        // without it, `pending` had no exit at all: a source that never becomes typed - `$a = [];`,
        // then a loop over `$a` - left the node in the tree, and PointerAdjuster's throw for a
        // survivor aborted the process on top of a perfectly good diagnostic nobody had printed yet
        void finalize();

    private:
        CodeRef code_ref_for(const TokenReference &token);

        // indexed rather than the base's ranged walk, because lower() replaces the child in place and
        // the wrapper it leaves behind is descended into on the same index - which is what lets a
        // nested foreach lower in the same round as the one around it
        void visitScope(ScopeNode &node) override;

        // a template's body is only meaningful once cloned into a concrete instance, and the source
        // type this pass needs is exactly what is not known there. OperatorRewriter's rule, and
        // PointerAdjuster's before it
        void visitFunctionDecl(FunctionDeclNode &node) override;

        // lowers `scope.children[index]`, which is a ForeachNode. leaves it in place when the source's
        // type is not settled yet, and replaces it with an empty scope when it refused - a refused node
        // must not survive, because run_semantic_passes runs PointerAdjuster *before* it gates on
        // has_critical_issues(), and a survivor turns a good located diagnostic into an unlocated
        // internal error stacked on top of it
        void lower(ScopeNode &scope, size_t index);

        // reports and discards, in that order - one function, because forgetting the discard is the
        // silent half and there are three arms that must not forget it
        void refuse(
            ScopeNode &scope, size_t index, ForeachNode &loop, const TokenReference &at, std::string why);

        void discard(ScopeNode &scope, size_t index, ForeachNode &loop);

        // `$__it->name()`, left unresolved for the fixpoint's own settle_calls to finish. a fresh
        // receiver subtree per call: PointerAdjuster rewrites edges in place, so a shared one would
        // collect a deref per use
        FunctionCallExprNode &iterator_call(
            VarDeclNode &iterator, const std::string &name, const TokenReference &at);

        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        bool _changed = false;

        // is this the finalizing round? see finalize() above. a flag rather than a parameter because
        // it has to reach lower() through the visitor's descent, which takes none
        bool _finalizing = false;
    };
};

#endif
