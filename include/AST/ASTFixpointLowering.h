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

    // **the chassis a pass running inside AST::Monomorphizer's fixpoint gets.**
    //
    // four passes rewrite or resolve the tree from inside the fixpoint - AST::ForeachLowering,
    // AST::GuardLowering, AST::InterpolationLowering and AST::MatchResolution - and only *which edge
    // they hook* is genuinely theirs. everything around it is this class: the walk, the position it
    // walks from, the exit obligation and the generic-body skip.
    //
    // it exists because the three invariants below must live in one place, and **none of them
    // breaks loudly**: forgetting one is a golden diff or a round that stalls, never a compile error.
    //
    //   - **finalize() is one more round, not a sweep.** a round inherits visitFunctionDecl's
    //     generic-body skip and walks scope children, which is the tree walk these passes are required
    //     to use: NodeCollection owns a detached node forever, so an `of_type` arena sweep would reach
    //     nodes the pass already lowered away and blame them.
    //   - **a hoist counter is per file**, so a golden's `$__guard0` does not move when an unrelated
    //     file above it grows one. owned here rather than left as a rule to remember - a pass asks
    //     next_hoist_index() and there is no reset to forget.
    //   - **`_changed` is what the fixpoint reads.** a pass that mutates the tree and does not set it
    //     stalls the round rather than failing, so the program compiles a round later or not at all.
    //
    // **what is deliberately not here.** no `lower()` virtual: the three hook different edges -
    // `visitScope` for the two that splice statements, `rewrite_value_edge` for the one that replaces an
    // expression - so a hook declared here would be a shape two of them do not have. and no reporting:
    // a refusal's wording and whether the node is kept or discarded are per pass, and the two answers
    // differ (AST::GuardLowering reports and *keeps*, the other two discard).
    //
    // **AST::OperatorRewriter is not built on this**, though it is the fourth pass in the fixpoint. its
    // pending state is a different shape, and its round ends by flushing the nodes it detached - which
    // this has no hook for and would have to grow one for. its `finalize()` is otherwise the same one
    // more round; what folding it in would cost is that hook plus generalising the pending state until
    // it described nothing. it keeps its own `_hoist_count`, and that is the one rule still written
    // twice.
    //
    // **not a `run_semantic_passes` pass**, so CLAUDE.md's mirror trap does not apply to anything built
    // on it: a subclass is a member of AST::Monomorphizer, and both the real pipeline and
    // tests/helpers.cpp get it by construction rather than by remembering to register it twice.
    class FixpointLowering : protected RecursiveVisitor
    {
    protected:
        explicit FixpointLowering(Bundle &bundle);

        // one walk of every file's root in every module. answers whether anything changed, so the
        // fixpoint can report progress - see `_changed` below, which is what a subclass sets
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

        // the next index for a local this pass mints - `$__guard0`, `$__guard1`. **per file**, reset by
        // run_round, which is the whole of the second invariant above
        size_t next_hoist_index();

        // a template's body is only meaningful once cloned into a concrete instance, and the type every
        // one of these passes needs is exactly what is not known there. AST::PointerAdjuster's rule
        void visitFunctionDecl(FunctionDeclNode &node) override;

        Bundle &_bundle;
        Collector &_collector;

        // where the walk is, so a subclass can mint nodes into the right arena and locate a diagnostic
        Module *_current_module = nullptr;
        File *_current_file = nullptr;

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
