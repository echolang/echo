#ifndef ASTFIXPOINTLOWERING_H
#define ASTFIXPOINTLOWERING_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTRecursiveVisitor.h"

#include "Token.h"

#include <cstddef>

namespace AST
{
    class Bundle;
    class Collector;
    class File;
    class FunctionDeclNode;
    class Module;

    // chassis for a pass inside the monomorphizer's fixpoint. subclasses hook an edge; this owns
    // the walk, the hoist counter, finalize, and `_changed`. none of those fail loudly:
    //
    //   - finalize() is one more round, not an `of_type` sweep (detached nodes stay in the arena)
    //   - hoist indices are per file, so a golden's `$__guard0` does not move
    //   - `_changed` is what the fixpoint reads: mutate and forget it, and the round stalls
    //
    // OperatorRewriter stays outside: its pending state is a different shape
    class FixpointLowering : protected RecursiveVisitor
    {
    protected:
        explicit FixpointLowering(Bundle &bundle);

        // one walk of every file root. answers whether anything changed, so the fixpoint can
        // report progress - see `_changed` below, which is what a subclass sets
        bool run_round();

        // **the fixpoint's exit obligation**: one last round in which "not settled yet" is a refusal.
        // being out of rounds is the proof that nothing was ever going to settle it -
        // Monomorphizer::finalize_calls' reasoning and its moment.
        //
        // a subclass with no pending state simply does not expose this. that is not an omission: it is
        // AST::InterpolationLowering, which never needs a type, so there is no "not decided yet" to
        // give an exit to
        void finalize();

        CodeRef code_ref_for(const TokenReference &token);

        // the next index for a local this pass mints - `$__guard0`, `$__guard1`. **per file**, reset
        // by run_round, which is the whole of the second invariant above
        size_t next_hoist_index();

        // a template's body is only meaningful once cloned into a concrete instance, and a t_owned
        // body is finished: the type every one of these passes needs is exactly what is not known
        // in a template, and minting into t_owned is an assert. AST::function_is_fixpoint_open
        void visitFunctionDecl(FunctionDeclNode &node) override;

        // save `_current_function`, descend, restore. the skip policy is the caller's
        void walk_function(FunctionDeclNode &node);

        Bundle &_bundle;
        Collector &_collector;

        // where the walk is, so a subclass can mint nodes into the right arena and locate a diagnostic
        Module *_current_module = nullptr;
        File *_current_file = nullptr;
        FunctionDeclNode *_current_function = nullptr;

        // **set by the subclass**, read by the fixpoint through run_round's answer. cleared at the head
        // of every round
        bool _changed = false;

        // is this the finalizing round? a flag rather than a parameter because it has to reach the
        // subclass through the visitor's descent, which takes none
        bool _finalizing = false;

    private:
        size_t _hoist_count = 0;
    };
};

#endif
