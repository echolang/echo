#ifndef ASTDESTRUCTION_H
#define ASTDESTRUCTION_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    // does a value of this type have to be destroyed when its owner goes out of scope?
    //
    // the transitive flag book/concept/ownership_and_moving.md asks for: "a struct that contains an
    // owner is itself an owner, and nothing needs to be declared for that". true when the type
    // declares a destructor, or when any of its properties needs one.
    //
    // **a class always answers true**, and answers it without looking at its properties. a class value
    // is one reference among possibly many, so what it owes at the end of a scope is a *release* - not
    // its payload's teardown, which happens later and only if that release turns out to be the last
    // one. the payload is a different question, and class_needs_deinit below is the one that asks it.
    // that split is also what terminates the recursion: `class Node { Node $next; }` is perfectly
    // ordinary, and a transitive walk over it would never bottom out.
    //
    // a **pointer is not an owner**. `ptr<uint8>` is an address and the type system knows nothing
    // about what is behind it, so nothing can be destroyed on its behalf - which is exactly why a
    // type holding one has to say what to do in a destructor. the same goes for a borrow `T&`: it
    // does not keep anything alive, so it certainly does not destroy anything. this is the one line
    // that makes the leaf case of every owning type explicit rather than accidental.
    //
    // answers false for a type parameter, because it is not a question that has an answer before
    // substitution - the ownership pass runs inside the monomorphizer's fixpoint and only ever asks
    // about a concrete type. asking early is not an error, it is a not-yet, and the caller gates on
    // AST::contains_type_param instead.
    //
    // deliberately **not cached** on ComplexType. `substituted_copy` is copy-then-modify, so a
    // cached bool would be carried onto every instantiation and be wrong the moment `Box<Buffer>`
    // and `Box<int32>` share a template. the recursion is shallow and terminates: a struct cannot
    // contain itself by value, a `ptr<Foo>` field stops at the pointer, and a class field stops at the
    // class.
    bool needs_destruction(const ValueType &type);

    // does a *class's payload* need tearing down when its last reference goes away, as opposed to just
    // being freed?
    //
    // true when the class declares a destructor, or holds a property that needs destroying - another
    // class to release, or a struct with a destructor of its own. this is what needs_destruction asks
    // of a struct, asked one level inside the block, and the answer decides whether the class gets a
    // synthesized deinit at all: a plain data class gets none, and its release is a decrement and a
    // free.
    //
    // takes the layout rather than a ValueType because every caller already holds one, and because
    // there is no meaningful answer for a non-class
    bool class_needs_deinit(const ComplexType *type);
};

#endif
