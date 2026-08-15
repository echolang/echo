#ifndef ASTINSTANTIATION_H
#define ASTINSTANTIATION_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <vector>

namespace AST
{
    class FunctionDeclNode;
    class FunctionCallExprNode;

    // how a template answers a call. declaration order is best to worst
    enum class InstantiationFit
    {
        // every type parameter came out bound to a concrete type and satisfies its constraint, so
        // the substitution can be used on the template's signature and the instance it names is
        // known
        t_yes,

        // nothing contradicted the template, but some parameter is not decided yet because an
        // argument had no type to bind it from. this is the ordinary state of a call written
        // *inside* an un-instantiated template body - `fac($n - 1)` where `$n` is `T` - which the
        // fixpoint answers later. the substitution is incomplete and must not be substituted with
        t_maybe,

        // the template cannot apply to these arguments at all
        t_no,
    };

    // what a diagnostic would blame, when there is a diagnostic to write
    //
    // this is a *separate* ordering from `fit`, over the same facts, and the difference is the
    // point: `fit` is asked by overload resolution, which only wants to know whether the candidate
    // is in or out, while `blame` is asked by the monomorphizer, which has to name something. the
    // two disagree about exactly one fact - an argument whose shape cannot be reconciled - see
    // `t_argument_shape`
    enum class InstantiationBlame
    {
        // nothing to blame
        n_none,

        // the call passes a different number of arguments than the template takes
        t_argument_count,

        // `foo<int32>(...)` named a different number of type arguments than the template has *own*
        // type parameters. a method's inherited prefix cannot be spelled at a call site, so it is
        // not counted here
        t_type_argument_count,

        // `unify_type` could not reconcile argument `argument` with its parameter, so no
        // substitution makes this argument fit
        //
        // ranked *last* of everything, and deliberately: an argument that does not fit is
        // AST::TypeChecker's diagnostic, reported against the substituted signature with the
        // argument's number in it. the other parameters may still decide the instance perfectly
        // well, which is why `type_arguments` can be filled while `fit` is `t_no` - the
        // monomorphizer instantiates through this blame rather than reporting it
        t_argument_shape,

        // `param` never got a binding, and no later round will give it one: it appears nowhere the
        // arguments could bind it from, and every argument that exists had a type to be read.
        // `pick<T, U>(T $x)` is the shape - `U` is unreachable
        t_unbound_parameter,

        // `param` is not decided *yet*: it is bound to something still mentioning a type parameter,
        // or unbound while an argument that could bind it has no type, or it belongs to a receiver
        // that is not resolved. this is the ordinary state of a call inside an un-instantiated
        // template body, and the reason a generic body is never instantiated as `Box<T>`
        t_undecided_parameter,

        // `param` is bound to `bound`, which its constraint does not allow
        t_constraint,
    };

    // the whole answer to "could a call with these arguments instantiate this template, and if not,
    // what is to blame"
    //
    // side-effect free by design: nothing here reports, records an issue or mutates a declaration,
    // which is what lets the parser ask it of a candidate it is about to discard. a template that
    // fails is simply not in the overload set, so a type constraint reads as an overload filter
    // rather than as an error the user has to work around - while the monomorphizer, asking the
    // same question of a call it has already committed to, reads the blame fields and reports
    struct Instantiation
    {
        InstantiationFit fit = InstantiationFit::t_no;
        InstantiationBlame blame = InstantiationBlame::n_none;

        // t_unbound_parameter, t_undecided_parameter, t_constraint
        const TypeParamDecl *param = nullptr;

        // t_constraint: what the parameter was bound to
        ValueType bound;

        // t_argument_shape: which argument, indexed into the call's argument list
        size_t argument = 0;

        // the identity of the instance this call names: every type parameter bound to a concrete
        // type, in *declaration* order, which is the order that identifies an instantiation.
        // unification binds in argument order, so this is not that order
        //
        // filled whenever every parameter is decided, **independently of `fit`** - see
        // InstantiationBlame::t_argument_shape for why those are different questions
        std::vector<ValueType> type_arguments;

        // is `type_arguments` the answer? false while any parameter is unbound or bound to
        // something not concrete
        bool decided = false;

        // what bound, keyed by declaration. complete only when `decided`
        TypeSubstitution bindings;
    };

    // **the owner's type parameters, for a call that has no receiver to read them from.**
    //
    // a method binds `Box<T>`'s T by unifying `args[0]`'s declared `Box<T>&` against the receiver's
    // type. a *static* has no args[0], and its owner's parameters may appear nowhere in its signature
    // at all - `result<T, E>::ok(T $v)` says nothing about E - so the owner the call site named is the
    // only thing that can say. without this the instantiation is undecidable, and undecidable is
    // reported by nobody: the monomorphizer skips a still-generic decl and determine_type_args answers
    // nullopt, both in silence, so the call is emitted nowhere and nothing explains it
    //
    // positional over the **inherited** prefix, which is exactly the owner's own parameter list -
    // FuncDeclParser shares the owner's declarations rather than copying them, so this binds the same
    // TypeParamDecl pointers the receiver path would have
    //
    // empty for anything that is not a static, and empty while `owner` still mentions a type parameter:
    // a static call written inside an un-instantiated template has to stay a not-yet, not a decision
    TypeSubstitution static_owner_bindings(const FunctionDeclNode *tmpl, const ValueType &owner);

    // the question asked with argument types alone, plus whatever type arguments the call site
    // spelled out. `explicit_type_args` empty means "infer everything"
    //
    // `static_owner` is the type a static call named - `unknown` for every other shape of call, which
    // is what makes the seed above a no-op for them
    //
    // **`argument_defers` says which arguments have no opinion about the instance's name.** an untyped
    // number literal is one: its type is a default nobody chose, and letting it bind a parameter that
    // another argument also mentions makes the argument that *knows* what it is fit the one that does
    // not - `pick(0, $n)` over a `usize $n` named `pick<int32>` and truncated `$n`. positional and
    // parallel to `argument_types`; empty means "every argument has an opinion", which is what the
    // type-only callers get and is the pre-existing rule
    Instantiation can_instantiate(
        const FunctionDeclNode *tmpl,
        const std::vector<ValueType> &argument_types,
        const std::vector<ValueType> &explicit_type_args = {},
        const ValueType &static_owner = ValueType::make_unknown(),
        const std::vector<bool> &argument_defers = {});

    // the same question asked of a call node: argument types are read off the arguments, type
    // arguments off `explicit_type_args`. for a caller that has not already got the argument types
    // in hand - AST::CallResolver has, since the matcher needs them too
    Instantiation can_instantiate(const FunctionDeclNode *tmpl, const FunctionCallExprNode &call);

    // how a call's types are read, in one place: an argument slot or type node left null by a failed
    // parse counts as unknown rather than as absent, so the vectors stay positional and every reader
    // agrees about what a hole means. shared by the call-node overload of `can_instantiate` and by
    // AST::CallResolver, which needs the argument types for the matcher as well
    std::vector<ValueType> argument_types_of(const FunctionCallExprNode &call);
    std::vector<ValueType> explicit_type_args_of(const FunctionCallExprNode &call);

    // and which of them are AST::is_untyped_literal, in the same positional shape. read off the nodes
    // here rather than at each caller so the two that ask can_instantiate cannot disagree about it
    std::vector<bool> argument_defers_of(const FunctionCallExprNode &call);

    // **the** constraint rule: the first (parameter, argument) pair the parameter's constraint
    // rejects, as an index into both vectors, or nothing when none does
    //
    // a bare type parameter is not a violation - it is judged once something is substituted for it.
    // shared with the struct-template mirror in Parser::parse_type, so `Vec<bool>` and
    // `only_numbers(true)` are rejected by one rule and differ only in the message they get
    std::optional<size_t> first_constraint_violation(
        const std::vector<TypeParamDecl *> &params,
        const std::vector<ValueType> &args);
};

#endif
