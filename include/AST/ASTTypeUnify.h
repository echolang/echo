#ifndef ASTTYPEUNIFY_H
#define ASTTYPEUNIFY_H

#pragma once

#include "AST/ASTValueType.h"

#include <vector>

namespace AST
{
    class FunctionDeclNode;

    // binds the type parameters `param` mentions to the matching parts of `arg`, returning false
    // when the two shapes cannot be reconciled at all.
    //
    // the bool is what lets overload resolution use this: with a single generic candidate it is
    // enough to bind what you can and notice afterwards that a parameter came out unbound, which
    // is all the monomorphizer ever needed. choosing *between* candidates needs to know that a
    // template does not apply, and "nothing bound" cannot say that - `at<T>(ptr<T>, usize)` binds
    // T fine from its first argument while its second is nonsense.
    //
    // `allow_decay` carries the pointer decay rule, which is a statement about the argument as a
    // whole rather than about every level of it: a pointer passed where a value is expected is
    // read, so `box($p)` binds T=int32. once a `ptr<T>` parameter has matched a pointer argument
    // structurally the caller has already opted out of that read, so the descent below it binds
    // exactly - otherwise `ptr<T>` against `ptr<ptr<int32>>` would bind T=int32 and hand the
    // instance an argument one level off
    bool unify_type(const ValueType &param, const ValueType &arg, TypeSubstitution &out, bool allow_decay = true);

    enum class InstantiationFit
    {
        // every type parameter came out bound and satisfies its constraint, so `out` can be used
        // to substitute the template's signature
        t_yes,

        // nothing contradicted the template, but some parameter is still unbound because an
        // argument had no type to bind it from. this is the ordinary state of a call written
        // *inside* an un-instantiated template body - `fac($n - 1)` where `$n` is `T` - and the
        // monomorphizer resolves it later. `out` is incomplete and must not be substituted with
        t_maybe,

        // the template cannot apply to these arguments at all
        t_no,
    };

    // "could a call with these argument types instantiate this template?" - unify_type over every
    // parameter, plus every one of the template's own type parameters coming out bound and
    // satisfying its constraint.
    //
    // side-effect free by design: it reports nothing and records no issue, which is what
    // separates it from Monomorphizer::determine_type_args. a candidate that fails here is simply
    // not in the overload set, so a type constraint reads as an overload filter rather than as an
    // error the user has to work around.
    //
    // the three-way answer is what keeps it usable during parsing: rejecting on "not everything
    // bound" would throw out every recursive call in a generic body, where the arguments still
    // mention the enclosing template's own parameters
    InstantiationFit can_instantiate(
        const FunctionDeclNode *tmpl,
        const std::vector<ValueType> &argument_types,
        TypeSubstitution &out);
};

#endif
