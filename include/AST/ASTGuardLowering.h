#ifndef ASTGUARDLOWERING_H
#define ASTGUARDLOWERING_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTUnwrap.h"

#include "Token.h"

#include <string>

namespace AST
{
    class Bundle;
    class Collector;
    class File;
    class FunctionCallExprNode;
    class GuardNode;
    class Module;
    class ScopeNode;
    class VarDeclNode;

    // **what a `guard` over a type of the author's own becomes.**
    //
    // a `T?` never reaches this pass at all: Parser::parse_guard decides that one, because a nullable's
    // payload is a property of the type and nothing later can change it. what is left is a subject that
    // declares `contract::unwrappable<V>`, which no parse could have answered - so this pass asks
    // AST::unwrap_plan_for once the fixpoint has settled the subject's type, and mints two calls:
    //
    //     <subject hoisted into an ordinary declaration ahead of the statement>
    //     guard  presence_test = $__guard0->has_value()
    //            bound_value   = deref($__guard0->unwrap())
    //            failure       = deref($__guard0->failure())    // only for `else ($e)`
    //
    // **its own pass rather than one more rule in AST::OperatorRewriter**, and that class's own header
    // says why: it holds "no second rule, only a second moment" of a decision the parser already made.
    // the protocol decision is not a second moment of anything - the parser cannot read a conformance,
    // which is the entire reason AST::unwrap_plan_for exists. AST::ForeachLowering ruled on this exact
    // question the same way for the same reason. mechanically it does not fit either: this pass splices a
    // declaration into the **enclosing** scope, and the rewriter's only splice wraps the statement in a
    // new scope - which is precisely the shape that would break the binding's lifetime.
    //
    // **inside the monomorphizer's fixpoint**, between the operator rewriter and the loop lowering:
    //   - *after* the rewriter, because it performs the weak upgrade that turns a substituted
    //     `weak<Node>` into a `Node?` (so the plan's nullable arm answers it), and because
    //     `$v = guard $slots[$i] else {...}` has no subject type until the bracket has become an
    //     `operator []` call
    //   - *before* the loop lowering, so a `foreach` over a guard's binding sees a typed binding in the
    //     same round
    //   - *before* the ownership pass, which walks a body exactly once ever - safe rather than merely
    //     early, because `body_is_concrete` answers false while `GuardNode::plan_decided` is false
    //
    // **not a `run_semantic_passes` pass**, so CLAUDE.md's mirror trap does not apply here: it is a
    // member of AST::Monomorphizer, and both the real pipeline and tests/helpers.cpp get it by
    // construction rather than by remembering to register it twice.
    class GuardLowering : private RecursiveVisitor
    {
    public:
        GuardLowering(Bundle &bundle);

        // answers whether anything changed, so the fixpoint can report progress
        bool run_round();

        // **the fixpoint's exit obligation**: one last round in which "the subject is not settled yet"
        // is a refusal. being out of rounds is the proof that nothing was ever going to settle it -
        // Monomorphizer::finalize_calls' reasoning and its moment
        void finalize();

    private:
        CodeRef code_ref_for(const TokenReference &token);

        // indexed rather than the base's ranged walk, because lower() *inserts* the hoisted subject
        // declaration ahead of the guard - so the walk has to see the list it left behind
        void visitScope(ScopeNode &node) override;

        // a template's body is only meaningful once cloned into a concrete instance, and the subject
        // type this pass needs is exactly what is not known there
        void visitFunctionDecl(FunctionDeclNode &node) override;

        // lowers `scope.children[index]`, which is a GuardNode whose plan is not decided. leaves it in
        // place when the subject's type is not settled yet
        void lower(ScopeNode &scope, size_t index);

        // **reports and keeps**, which is the opposite of what AST::ForeachLowering does and the
        // difference is the form: a loop's `$el` is read *inside* it, so discarding the node takes
        // every reader with it - while a guard's binding is read *after* it, so forgetting the subtree
        // would free a declaration those reads still point at, which is the unsound direction of
        // NodeCollection::forget. so the node stays, the binding is typed defensively, `plan_decided`
        // is set so nothing asks again, and has_critical_issues() stops the build before codegen
        void refuse(GuardNode &guard, const TokenReference &at, std::string why);

        // **the binding's type, inferred or checked** - one rule for both arms of lower(), the two
        // moments being a `T?` the fixpoint only just settled and a payload only a conformance could
        // give. false when it refused, so a caller stops rather than carrying on with a binding whose
        // type it just contradicted. Parser::parse_guard is the third moment and reports differently -
        // AST::guard_payload_refusal is the wording all three share
        bool bind_payload_type(
            GuardNode &guard,
            const ValueType &payload,
            const ValueType &subject
        );

        // `$__guardN->name()`, through AST::make_unresolved_member_call - which owns the receiver rule
        // and the settlement, shared with AST::ForeachLowering. a **fresh** receiver subtree per call:
        // AST::PointerAdjuster rewrites edges in place, so a shared one would collect a deref per use
        FunctionCallExprNode &subject_call(
            VarDeclNode &subject,
            const std::string &name,
            const TokenReference &at
        );

        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        bool _changed = false;
        bool _finalizing = false;

        // **per file**, so a golden can tell two hoists apart and the numbering does not move when an
        // unrelated file above grows one - AST::OperatorRewriter's `_hoist_count` rule
        size_t _hoist_count = 0;
    };
};

#endif
