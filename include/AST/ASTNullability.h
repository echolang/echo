#ifndef ASTNULLABILITY_H
#define ASTNULLABILITY_H

#pragma once

#include "AST/ASTValueType.h"
#include "Token.h"

namespace AST
{
    class ExprNode;
    class Module;
    class ScopeNode;

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

    // **does control always leave this scope?** true when its last statement is a `return` or a `die`.
    //
    // `guard`'s else block is the one thing that asks, and it has to: a guard binds a name that is only
    // meaningful on the path where the value was there, so an else arm that ran on and rejoined would
    // leave that name bound to nothing. refusing at the declaration is what makes the binding's promise
    // true by construction rather than by the author remembering
    //
    // deliberately shallow - the *last* statement, not a walk of every path. an `if` whose two arms both
    // return is not recognised, and that is a limitation worth having on purpose: the alternative is a
    // reachability analysis, and this language has no `break` or `continue` yet for one to be complete
    // over. it answers the shapes an else arm is actually written in
    //
    // lives here because `guard` is its only caller. if a second one appears - an exhaustive-match arm, a
    // never-returning call in statement position - it wants its own header and a row in CLAUDE.md
    bool scope_always_exits(const ScopeNode &scope);
};

#endif
