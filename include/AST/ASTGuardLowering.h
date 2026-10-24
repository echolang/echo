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
    class ExprNode;
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class GuardNode;
    class ScopeNode;
    class VarDeclNode;

    // `guard` over `contract::unwrappable` / `checkable`. a binding `T?` is decided in the parser;
    // statement-form `T?` only hoists a short-lived subject. unwrap lands on the binding's
    // `init_expr`, never `bound_value` (that double-frees an owning payload). callees come off
    // UnwrapPlan, never by name. inside the fixpoint: after OperatorRewriter, before ForeachLowering
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
        // **not a name**: minting `"has_value"`, `"unwrap"` and `"failure"` as unresolved by-name
        // calls would let AST::CallResolver choose all over again - a second answer to which
        // declaration the conformance had accepted, over a requirement whose *spelling* in
        // stdlib/core/contract.eco would then be load-bearing in a compiler pass.
        //
        // a **fresh** receiver subtree per call: AST::PointerAdjuster rewrites edges in place, so a
        // shared one would collect a deref per use
        FunctionCallExprNode &subject_call(
            VarDeclNode &subject,
            FunctionDeclNode *callee,
            const TokenReference &at
        );

        // **`$__guardN` spliced ahead of the guard**, typed and initialized through
        // AST::seat_receiver_local. protocol and statement-form T? share it so a place that is
        // already an address is not wrapped twice in one path and owned in the other, and a
        // place rooted in a temporary (`Box()->n`) is copied rather than addressed
        VarDeclNode &hoist_subject(
            ScopeNode &scope,
            size_t index,
            GuardNode &guard,
            ExprNode *expr
        );
    };
};

#endif
