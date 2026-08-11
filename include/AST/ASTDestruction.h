#ifndef ASTDESTRUCTION_H
#define ASTDESTRUCTION_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    // does a value of this type have to be destroyed when its owner goes out of scope?
    //
    // the transitive flag book/concept/ownership_and_moving.md asks for: true when the type declares
    // a destructor, or when any of its properties needs one
    //
    // **a class always answers true, without looking at its properties.** a class value is one
    // reference among many, so what it owes at a scope end is a *release*; the payload's teardown is
    // a later and separate question, asked by needs_deinit below. that split is what
    // terminates the recursion - `class Node { Node $next; }` is ordinary Echo
    //
    // **a weak reference also always answers true**, and it is worth being clear about what it owes.
    // it does not own its object - that is what it is for - but it does hold the block readable, and
    // that is a count somebody has to give back. so a `weak<T>` is destroyed at scope end exactly as a
    // class is; what differs is which of the two counts moves
    //
    // **a pointer is not an owner**, nor is a borrow `T&`: an address says nothing about what is
    // behind it, which is exactly why a type holding one must spell out a destructor. this is the
    // one line that makes the leaf case of every owning type explicit
    //
    // a type parameter answers false - a not-yet rather than an error. callers gate on
    // AST::contains_type_param, and the ownership pass only ever asks about concrete types
    //
    // deliberately **not cached** on ComplexType: an instantiation reaches most of its template
    // through the template_or_self redirect rather than owning a copy, so a cached bool would be
    // read off the one template shared by `Box<Buffer>` and `Box<int32>`
    bool needs_destruction(const ValueType &type);

    // does a value *of this layout* owe a teardown - as opposed to a class handle, which owes a release
    // and asks this question later?
    //
    // true when the type declares a destructor, or holds a property that needs destroying - another
    // class to release, or a struct with a destructor of its own. that is the same walk
    // needs_destruction performs for a struct, and for a class it is that walk asked one level inside
    // the block, at the moment the last reference goes away.
    //
    // **one question and one answer for both storage classes**, because there is one teardown:
    // AST::OwnershipPass::ensure_deinit reads this, and what it answers is the single function that
    // tears a value of the type down. A class reaches that function from its release thunk when the
    // strong count hits zero; a struct reaches it from the drop the ownership pass wrote at the end of
    // the value's scope. Which is also the whole of what "a plain data class gets no deinit" means -
    // its release is a decrement and a free.
    //
    // takes the layout rather than a ValueType because every caller already holds one, and because the
    // per-level flags a *use* of the type carries have nothing to say about it
    bool needs_deinit(const ComplexType *type);

    // does that teardown have to reach *inside* the value - as opposed to being a call to a destructor
    // its author wrote?
    //
    // the other half of needs_deinit above, which is this or a declared destructor. it is a question of
    // its own because it decides whether there is a body to synthesize at all: a type whose destructor is
    // the whole of its teardown needs no synthesized member, and the drop site calls what the author
    // wrote. And because reaching inside is the thing that has to happen in a body the type owns - a
    // member access minted anywhere else is refused by `private`, which is what made an owning private
    // property unusable
    bool properties_need_destruction(const ComplexType *type);
};

#endif
