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
    // a later and separate question, asked by class_needs_deinit below. that split is what
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

    // does a *class's payload* need tearing down when its last reference goes away, as opposed to just
    // being freed?
    //
    // true when the class declares a destructor, or holds a property that needs destroying - another
    // class to release, or a struct with a destructor of its own. this is what needs_destruction asks
    // of a struct, asked one level inside the block, and the answer decides whether the class gets a
    // synthesized deinit at all: a plain data class gets none, and its release is a decrement and a
    // free
    //
    // takes the layout rather than a ValueType because every caller already holds one, and because
    // there is no meaningful answer for a non-class
    bool class_needs_deinit(const ComplexType *type);
};

#endif
