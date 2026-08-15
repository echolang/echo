#ifndef ASTGUARDLOWERING_H
#define ASTGUARDLOWERING_H

#pragma once

#include "AST/ASTFixpointLowering.h"
#include "AST/ASTUnwrap.h"

#include "Token.h"

#include <string>

namespace AST
{
    class Bundle;
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class GuardNode;
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
    //     guard  <decl>->init_expr = deref($__guard0->unwrap())
    //            presence_test     = $__guard0->has_value()
    //            failure           = deref($__guard0->failure())    // only for `else ($e)`
    //
    // **the unwrap goes on the declaration's own `init_expr` and never on `GuardNode::bound_value`**, and
    // that is the whole of what this pass owes the binding: AST::OwnershipPass::resolve_value_arrival then
    // sees a value arriving at a declaration and covers it with no arm at all. writing it onto
    // `bound_value` gave that edge two producers with two rules, and the pass that owns "what does an
    // arriving value owe" knew about one - so the payload was byte-copied out of storage the subject still
    // owned. AST::ForeachLowering lowers `$el` into exactly this shape for exactly this reason.
    //
    // **the callees come off AST::UnwrapPlan, never by name.** the conformance already chose which
    // declaration answers each requirement, and asking the matcher again is a second answer to that -
    // reachable, since `has_value()` is const and `unwrap()` is not, so the two differ in the axis
    // AST::argument_fit ranks on. AST::IterationPlan states the rule and carries its `iterate` for it.
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
    // **the walk, the exit obligation and the generic-body skip are AST::FixpointLowering's**, which is
    // also where the three invariants they rest on are written. what is left here is the two things that
    // are genuinely this pass's: which edge it hooks, and what a guard lowers to.
    //
    // **not a `run_semantic_passes` pass**, so CLAUDE.md's mirror trap does not apply here: it is a
    // member of AST::Monomorphizer, and both the real pipeline and tests/helpers.cpp get it by
    // construction rather than by remembering to register it twice.
    class GuardLowering : private FixpointLowering
    {
    public:
        GuardLowering(Bundle &bundle);

        // the chassis' own two entry points, and the whole of this pass's public surface. `finalize` is
        // exposed because a guard *has* a pending state - a subject the fixpoint has not settled - which
        // is exactly what AST::InterpolationLowering does not
        using FixpointLowering::run_round;
        using FixpointLowering::finalize;

    private:
        // indexed rather than the base's ranged walk, because lower() *inserts* the hoisted subject
        // declaration ahead of the guard - so the walk has to see the list it left behind
        void visitScope(ScopeNode &node) override;

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

        // **`$__guardN->f()` where `f` is the declaration the conformance answered with**, off
        // AST::UnwrapPlan, through AST::make_resolved_member_call - which owns the receiver rule and the
        // settlement, shared with AST::ForeachLowering's `iterate()`.
        //
        // **not a name**: this pass used to mint `"has_value"`, `"unwrap"` and
        // `"failure"` as unresolved by-name calls and let AST::CallResolver choose all over again -
        // a second answer to which declaration the conformance had accepted, over a requirement whose
        // *spelling* in stdlib/core/contract.eco was therefore load-bearing in a compiler pass.
        //
        // a **fresh** receiver subtree per call: AST::PointerAdjuster rewrites edges in place, so a
        // shared one would collect a deref per use
        FunctionCallExprNode &subject_call(
            VarDeclNode &subject,
            FunctionDeclNode *callee,
            const TokenReference &at
        );
    };
};

#endif
