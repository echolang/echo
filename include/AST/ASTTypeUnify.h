#ifndef ASTTYPEUNIFY_H
#define ASTTYPEUNIFY_H

#pragma once

#include "AST/ASTValueType.h"

#include <vector>

namespace AST
{
    // binds the type parameters `param` mentions to the matching parts of `arg`, returning false
    // when the two shapes cannot be reconciled at all
    //
    // the shape question only. whether the arguments *decide* an instantiation, and what to blame
    // when they do not, is AST::can_instantiate's (AST/ASTInstantiation.h) - this one has no
    // opinion about a parameter it could not reach. the bool matters to both of that function's
    // readers: "nothing bound" cannot say that a template does not apply, and `at<T>(ptr<T>, usize)`
    // binds T fine from its first argument while its second is nonsense
    //
    // `allow_decay` carries the two call-boundary rules that read or write one pointer level, both
    // statements about the argument as a whole rather than about every level of it: a pointer passed
    // where a value is expected is read (`box($p)` binds T=int32), and a place passed to a borrow
    // has its address taken (`bump($a)` binds T=int32 through a `T&` parameter). once a `ptr<T>`
    // parameter has matched a pointer argument structurally the caller has opted out of both, so the
    // descent below it binds exactly - otherwise `ptr<T>` against `ptr<ptr<int32>>` would bind
    // T=int32 and hand the instance an argument one level off
    bool unify_type(const ValueType &param, const ValueType &arg, TypeSubstitution &out, bool allow_decay = true);
};

#endif
