#ifndef ASTUNWRAP_H
#define ASTUNWRAP_H

#pragma once

#include "AST/ASTValueType.h"
#include "Token.h"

#include <optional>
#include <string>

namespace AST
{
    class CoreTypes;
    class FunctionDeclNode;

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

        // **the two callees, named through the conformance rather than by their spelling** - the rule
        // IterationPlan::iterate states. null on the t_builtin_nullable arm,
        // where the compiler answers the presence question itself and there is nothing to call.
        //
        // this matters three ways, and only the first is tidiness. `stdlib/core/contract.eco` could
        // rename everything it declares and only Echo source would notice, which is precisely what
        // `#[core:]` exists to give. `has_value()` is const and `unwrap()` is not, so the two already
        // differ in the axis AST::argument_fit ranks on - a by-name lookup could land on a
        // differently-ranked overload than the one conformance checking accepted. and a user type
        // declaring `unwrappable<V>` beside its own unrelated `unwrap(...)` set was resolved by the
        // matcher rather than by the contract.
        //
        // for an *erased* subject these are the interface's own requirements, which is not a second case:
        // a requirement **is** the declaration there, and ExprCodegen::gen_function_call already routes
        // on FunctionDeclNode::is_interface_requirement()
        FunctionDeclNode *has_value = nullptr;
        FunctionDeclNode *unwrap = nullptr;

        // E, when the subject declares `contract::failable<E>`. **absent is not an error** until an
        // `else ($e)` asks for one, and that refusal belongs at the `$e` - exactly
        // IterationPlan::key_type's rule and for the same reason
        std::optional<ValueType> failure_type;

        // `failure()`, set exactly when `failure_type` is. **the two answers are one answer**: reading E
        // off the conformance while leaving whether the declaration exists to be discovered by a name
        // lookup inside the lowering, one pass later, was the worse half of the same bug
        FunctionDeclNode *failure = nullptr;
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

        // **`t_pending` for a reason somebody else's diagnostic already owns**, so the asker's finalizing
        // round stays quiet instead of inventing a sentence.
        //
        // a conformance that is *declared and unanswered* is the one case: this function has nothing to
        // lower and never will, but the mistake is at the `struct` and AST::TypeChecker reports it there,
        // as an UnmetInterfaceRequirement naming the requirement the author owes. that pass runs after
        // the monomorphizer's fixpoint, so `Collector::has_critical_issues()` - the asker's existing
        // answer to "did anything else explain this" - cannot see it yet, and without this flag the
        // guard refused with "'cell' never got a type", about a type that is perfectly well settled.
        //
        // never set beside `t_ok` or `t_refused`: it is what a pending answer says about *itself*
        bool reported_elsewhere = false;
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

    // the unspellable `#[builtin: unwrap_abort]` a `guard` without `else` lowers to. one per
    // module, created on first use. `--no-stdlib` gets the same decl: a `T?` without an arm
    // must still stop
    FunctionDeclNode &ensure_unwrap_abort(class Module &module, const TokenReference &at);
};

#endif
