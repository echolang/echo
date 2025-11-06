#ifndef ASTNULLABILITY_H
#define ASTNULLABILITY_H

#pragma once

#include "AST/ASTValueType.h"
#include "Token.h"

#include <string>

namespace AST
{
    class ExprNode;
    class Module;
    class NullNode;

    // **the three forms that read a value out of something that may not have one** - `guard`, `??` and
    // `?->` - and the one question all three ask first.
    //
    // they are nullability features, not weak features. every one of them works on any `T?` whatever `T`
    // is: a nullable primitive, struct, class, interface or callable, and a `ptr<T>`, which is this same
    // flag on a pointer level. none of their arms mentions a class, and that is the point of having built
    // `T?` as a flag rather than as a class-shaped special case - `guard int32 $v = lookup($k) else {...}`
    // and `guard Node $n = $weak else {...}` are one implementation.

    // **is this operand certainly there?** asked *after* optional_operand_of, so a weak has already
    // become a nullable and the question is only about the flag.
    //
    // all three forms refuse an operand this answers true for - a guard that cannot fail, a `??` whose
    // right side could never be reached, a `?->` that could never short circuit - each against its own
    // wording, which is why this is the predicate and not the diagnostic. an undetermined type and a bare
    // type parameter answer false: they are "ask again later", and judging them here is a round too early
    bool is_certainly_present(const ValueType &type);

    // which of the three was written. the mistakes are not the same mistake - "a guard that can never
    // fail", "a `??` whose left side is always there" and "a `?->` that could never short circuit" are
    // three different things to tell an author - so the wording is per form even though the rule is not
    enum class OptionalForm
    {
        t_guard,
        t_null_coalesce,
        t_optional_chain,
    };

    // **the diagnostic is_certainly_present earns, worded for the form it was written in.** empty when
    // there is nothing to refuse, so an asker may call it without asking the predicate first.
    //
    // there are *two* moments, not one, and that is why this exists rather than three string literals
    // at three sites. the parser asks, where the diagnostic is best located and where most programs are
    // decided; and AST::TypeChecker asks again after the monomorphizer, because inside a template the
    // operand is a bare `T` and the predicate correctly answers "later" - see todo/B27. two askers per
    // form is exactly how three wordings become six and then drift
    std::string certainly_present_refusal(OptionalForm form, const ValueType &operand_type);

    // the expression the three forms should actually branch on, given the one that was written.
    //
    // hands back `expr` unchanged when it is already nullable, and wraps it in a `StrongExprNode` when it
    // is a `weak<T>` - so `$weak?->name()`, `$weak ?? $other` and `guard $n = $weak` all read the way they
    // look, by upgrading first. **the single place "a weak is also accepted here" is decided**, so the
    // three cannot drift apart, and everything downstream of it sees a plain nullable and nothing else.
    //
    // returns `expr` unchanged for a non-optional operand too. that is not this function's error to
    // report - each form refuses it against its own wording, because "a guard that can never fail" and
    // "a `??` whose left side is always there" are different mistakes
    //
    // the upgraded handle is a value with no storage, which AST::OwnershipPass::bind_pending_temporaries
    // already owns - so it lives for the expression and is released after, with no new lifetime rule
    //
    // `at` is the *form's* own token - the `guard`, the `??`, the `?->` - so a diagnostic about an upgrade
    // nobody wrote points at the thing that needed one
    ExprNode *optional_operand_of(ExprNode *expr, Module &module, const TokenReference &at);

    // the type a form yields once the operand has been unwrapped: the non-null of a nullable, and the
    // class named by a weak. asked *after* optional_operand_of, so the weak case is already a nullable by
    // the time it gets here - it is spelled out anyway, because a caller reading a type off the operand it
    // was handed rather than off the rewritten one is an easy and silent mistake
    ValueType unwrapped_type_of(const ValueType &type);

    // **"did the user write `null` here?"** - the entry condition every null rule shares, and the one
    // question about a null that is about the *node* rather than about a type: `null` has no type of its
    // own, so which operand was one cannot be read off a ValueType.
    //
    // it looks through the implicit casts the parser and the monomorphizer wrap around an argument,
    // because the null-specific rules all test for the raw `n_null` tag and a cast inserted to reconcile
    // an argument with its parameter hides that tag behind an `n_type_cast`.
    //
    // spelled once so an arrival site cannot get the rule right and the question wrong - which had already
    // happened three times over: AST::TypeChecker stripped, AST::binary_has_builtin_meaning's operand
    // builders did not, and ExprCodegen's `== null` arm did not either, so a cast-wrapped null was one
    // thing to the checker and another to the two gates that decide what the comparison even means
    bool is_written_null(const ExprNode *expr);

    // the same walk, handing back the node instead of a verdict - so a site that has to *write* to the
    // null does not re-derive which expressions are ones. `bind_null_to` is the usual way to write one;
    // this is for the caller that binds against something other than a plain destination type, which
    // today is only AST::PointerAdjuster's comparison widening
    NullNode *written_null_of(ExprNode *expr);

    // **does a written `null` belong at this destination?** the companion to is_written_null above: that
    // one is the question about the node, this one is the question about the type it is arriving at.
    //
    // one question, and it used to be spelled by hand at every asker with the spellings already drifted:
    // the expression parser said `is_nullable() || is_weak()`, the return parser said the same inverted
    // and folded into its literal hint gate, and PointerAdjuster said `is_nullable() || is_class() ||
    // is_weak()`. a fourth asker - AST::CallResolver, for an argument - is what made the drift worth
    // removing rather than describing. most of them now reach it through `bind_null_to`; the direct
    // callers are the return parser's hint gate and AST::null_rejection_reason, which need the answer
    // without a node to bind
    //
    // a `ptr<T>` needs no arm: nullability is a per-level flag on any kind, so a pointer that admits null
    // *is* `is_nullable()` on its pointer level
    bool destination_admits_null(const ValueType &type);

    // **gives a written `null` the type of the place it is going.** `null` has no type of its own, and the
    // destination decides more than its name: an address-like nullable is a null pointer, a wrapped `T?` is
    // a cleared tag, and a null that never learned which it was reaches codegen as the former and is then
    // wrapped as though it were present.
    //
    // a no-op on anything that is not a written null, on a null that is already bound, and on a destination
    // that does not admit one - so an asker may call it without first knowing which of those it has. an
    // unbound null left behind is deliberate: AST::null_rejection_reason is what reports it, against the
    // destination, and doing that here would report the same mistake twice
    //
    // it reaches *through* an implicit cast for the same reason is_written_null does, so the two agree on
    // what counts as a null: whenever is_written_null(expr) holds and the destination admits one, this
    // binds it. returns true when the null now carries a type
    bool bind_null_to(ExprNode *expr, const ValueType &destination);

    // **what an operator says about a `null` operand it has no overload for.** one wording with two
    // askers, because the two arrive from opposite directions and would otherwise drift: AST::CallResolver
    // reaches it when the operator has several overloads and none of them fits, AST::TypeChecker when it
    // has one, which the matcher takes without consulting types at all and the coercion then refuses.
    //
    // deliberately says nothing about a parameter type. every `operator` declaration in the program shares
    // the root namespace, so naming the losing candidate's parameter tells the author about a type no file
    // of theirs mentions - `$p == null` on a struct answered with a sentence about 'const string&'
    std::string null_operand_refusal(const std::string &operator_spelling);

    // `guard`'s "the else arm must leave" rule used to live here, back when `guard` was its only caller.
    // it is AST::scope_always_exits ([AST/ASTControlFlow.h]) now - control flow is not a nullability
    // question, and it has a second asker
};

#endif
