#ifndef ASTUNWRAP_H
#define ASTUNWRAP_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    class CoreTypes;

    // where the value behind a `guard` comes from
    enum class UnwrapSource
    {
        // a `T?` - the per-level flag over an address, or the interned `{ __has, __value }` pair. no
        // protocol, no calls and no core-type binding: this is what `guard` meant before the unwrapping
        // protocol existed and it stays exactly that, which is what keeps every program written against
        // it byte for byte and keeps `guard` working under `--no-stdlib`
        t_builtin_nullable,

        // the subject declares `contract::unwrappable<V>`, or *is* an erased application of it.
        // **one kind and not two**, because nothing downstream differs: both are answered by two member
        // calls on the subject, and whether those dispatch directly or through a vtable is
        // ExprCodegen::gen_function_call's question, asked of
        // FunctionDeclNode::is_interface_requirement(). the same call `IterationSource` makes about its
        // own erased arm
        t_protocol,
    };

    struct UnwrapPlan
    {
        UnwrapSource kind = UnwrapSource::t_builtin_nullable;

        // V - what the binding holds, **carrying its own const**, for IterationPlan::element_type's
        // reason: it is the difference a const subject would make, and the place a future
        // `const_unwrappable` would be read
        ValueType payload_type;

        // E, when the subject declares `contract::failable<E>`. **absent is not an error** until an
        // `else ($e)` asks for one, and that refusal belongs at the `$e` - exactly
        // IterationPlan::key_type's rule and for the same reason
        std::optional<ValueType> failure_type;
    };

    // **the sole answer to "how is this value unwrapped".** the unwrapping protocol's
    // AST::iteration_plan_for, and modelled on it deliberately: a reader who knows one knows this.
    //
    // three results and not a bool, for AST::can_instantiate's reason: `t_pending` is what a subject the
    // fixpoint has not settled yet must get, and it has to be distinguishable from a refusal - otherwise
    // a guard reports "'[unknown]' is always present" once per round and the first round's guess becomes
    // the diagnostic.
    //
    // **reports nothing itself.** it has no CodeRef and the caller does - the same split
    // AST::iteration_plan_for and AST::interface_erasure_refusal make, and for the same reason: the
    // token a refusal should point at is the asker's, not this function's business.
    struct UnwrapLookup
    {
        enum class Result
        {
            t_ok,

            // ask again next round: the subject's type is not settled, or its conformance is not filled
            // in yet
            t_pending,

            // this cannot be unwrapped, and `refusal` says why in the caller's words
            t_refused,
        };

        Result result = Result::t_pending;
        UnwrapPlan plan;
        std::string refusal;
    };

    // **`subject.is_nullable()` is the first arm, ahead of the unbound-core check** - which is the
    // opposite of AST::iteration_plan_for's order, and the difference is the point. `foreach` has no
    // meaning at all without the iteration protocol, so a missing binding must be a located refusal.
    // `guard` had a meaning before the unwrapping protocol existed and keeps it: a `T?` must still guard
    // when the standard library is left out, and `stdlib/core/contract.eco` itself has to stay
    // compilable by a compiler that would otherwise be asking about the interfaces it is mid-parse of.
    //
    // that arm also short-circuits a `T?` that only became one after substitution, which is what makes
    // the deferred path answer identically to the immediate one.
    UnwrapLookup unwrap_plan_for(const ValueType &subject, const CoreTypes &core, TypeRegistry &types);

    // **the declared type against what is actually inside**, worded once for its two moments:
    // Parser::parse_guard for a `T?`, where the payload is known as the statement is read, and
    // AST::GuardLowering for a subject whose payload only a conformance could give.
    //
    // empty when there is nothing to refuse, so an asker calls it without asking a predicate first
    std::string guard_payload_refusal(
        const ValueType &written,
        const ValueType &payload,
        const ValueType &subject
    );
};

#endif
