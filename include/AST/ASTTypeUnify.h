#ifndef ASTTYPEUNIFY_H
#define ASTTYPEUNIFY_H

#pragma once

#include "AST/ASTValueType.h"

#include <vector>

namespace AST
{
    // **where in the parameter's shape a match sits, which is what decides what a `const` on the
    // argument means.** the two answers are not degrees of one thing:
    //
    //  - on a **level** - the parameter itself, or one reached through a pointer or a weak - a `const`
    //    describes the *place* the value was read from. `f(const K& $key)` passing `$key` on binds
    //    K=string, because the value is a string and only the borrow was read-only.
    //
    //  - inside a generic application's **argument list**, a `const` is part of the instantiation's
    //    identity. `slice<const int32>` is a window over storage it may not write, and that is a fact
    //    about the type rather than about the place it was found in - stripping it makes the
    //    substituted parameter `slice<int32>`, a different type from the argument that produced it.
    //
    // **sticky once set**, so `slice<ptr<T>>` against `slice<ptr<const int32>>` binds T=const int32:
    // every level below a type argument is still describing that type
    enum class UnifyPosition
    {
        t_level,
        t_type_argument,
    };

    // binds the type parameters `param` mentions to the matching parts of `arg`, returning false
    // when the two shapes cannot be reconciled at all
    //
    // The shape question only. Whether the arguments *decide* an instantiation, and what to blame when
    // they do not, is AST::can_instantiate's (AST/ASTInstantiation.h). This one has no opinion about a
    // parameter it could not reach.
    //
    // The bool matters to both of that function's readers: "nothing bound" cannot say that a template
    // does not apply, and `at<T>(ptr<T>, usize)` binds T fine from its first argument while its second
    // is nonsense.
    //
    // `allow_decay` carries the two call-boundary rules that read or write one pointer level. Both are
    // statements about the argument as a whole rather than about every level of it: a pointer passed
    // where a value is expected is read (`box($p)` binds T=int32), and a place passed to a borrow has
    // its address taken (`bump($a)` binds T=int32 through a `T&` parameter).
    //
    // Once a `ptr<T>` parameter has matched a pointer argument structurally, the caller has opted out of
    // both, so the descent below it binds exactly. Otherwise `ptr<T>` against `ptr<ptr<int32>>` would
    // bind T=int32 and hand the instance an argument one level off
    bool unify_type(const ValueType &param, const ValueType &arg, TypeSubstitution &out, bool allow_decay = true, UnifyPosition position = UnifyPosition::t_level);
};

#endif
