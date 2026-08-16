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
    // operand is a bare `T` and the predicate correctly answers "later". two askers per
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

    // **the mirror of the above is not here**: `T` -> `T?` is AST::TypeRegistry::get_or_create_optional,
    // and it takes a registry rather than being a static on ValueType because a tagged optional is a *type*
    // and a type has to be interned. That function is the one place the two spellings of a nullable are
    // chosen between, so nothing else has to know that a `T?` is a flag over an address and an interned
    // pair over anything else - and a second name in front of it would be a second answer to grep for
    //
    // **does a value arriving at `to` have to be wrapped into an absence?** the one question five
    // sites ask, and they have to agree or a program is accepted by resolution and mis-lowered by
    // codegen: AST::argument_fit peels the destination to rank the arrival,
    // AST::TypeChecker::check_destination_fits peels it to accept one, AST::CallResolver mints the cast and
    // TypeLowering::coerce_value emits the two `insertvalue`s.
    //
    // an argument that is *already* nullable is excluded: there it is the pair itself that arrives, which
    // an equality answers, and peeling would compare an absence against a payload.
    //
    // shaped like AST::implicit_conversion_target - its own rule with readers, rather than a peel spelled
    // at each - and here rather than in ASTArgumentFit.h because codegen reads it too and must not depend
    // on the overload ranking to do so
    bool arrival_wraps_optional(const ValueType &from, const ValueType &to);

    // **the type the value itself arrives at**, which is the payload wherever the answer above is yes.
    //
    // the peel, and not only the predicate. sharing the question and respelling the answer is what let a
    // reader forget it: AST::OwnershipPass classified the *copy* against the pair rather than the
    // payload, built a copy constructor whose parameter is `const T?&`, and reported "cannot implicitly
    // convert 'string&' to 'const string?&'" at a `return` - a sentence about a type nobody wrote. that
    // is the shape AST::implicit_conversion_target has for the same reason, and having it here means a
    // fifth reader cannot ask the question and then get the answer wrong
    ValueType arrival_destination_of(const ValueType &from, const ValueType &to);

    // **what `echo` prints when handed a `T?`** - the payload, and only when the payload is a primitive.
    //
    // the peel `echo` has always performed: the flag spelling made an `int32?` answer is_primitive() and
    // coerce_value unboxed it, so this keeps the emitted IR the same now that the pair is a layout. a
    // payload that is not a primitive is handed back untouched and refused by whoever asked, because the
    // only other thing `echo` can print is a string and that path is a length-counted write over the
    // value's own two words, which would read a tagged pair as one.
    //
    // **one owner because two halves read it**: AST::TypeChecker's echo arm decides whether the program is
    // accepted and Compiler::LLVM::printf_conversion_for decides what is emitted. spelled twice, the
    // permissive direction is an internal compiler error thrown at a user program rather than a diagnostic
    ValueType echo_printed_type_of(const ValueType &type);

    // what an `A?->b` answers, from the continuation the chain reaches.
    //
    // stored on the node rather than derived at every ask (AST::OptionalChainExprNode::result), because
    // wrapping needs the registry and `result_type()` has no context. shared by the two that write it -
    // the parser at construction and AST::OperatorRewriter each round - so the refresh cannot drift from
    // the original
    //
    // takes the **node**, not its type, so the null-continuation case is answered here too rather than by
    // a ternary each writer spells for itself - which is half a derivation living outside the function
    // that exists to own it
    ValueType optional_chain_result_type(const ExprNode *continuation, TypeRegistry &registry);

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
    // one question. most askers reach it through `bind_null_to`; the direct callers are the return
    // parser's hint gate and AST::null_rejection_reason, which need the answer without a node to
    // bind. a second spelling (`is_nullable() || is_weak()` here, `is_nullable() || is_class() ||
    // is_weak()` there) is a program accepted by one site and refused by another
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

    // `guard`'s "the else arm must leave" rule is AST::scope_always_exits ([AST/ASTControlFlow.h]) -
    // control flow is not a nullability question, and it has a second asker
};

#endif
