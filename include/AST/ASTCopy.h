#ifndef ASTCOPY_H
#define ASTCOPY_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    class FunctionDeclNode;

    // the copy constructor for a value of this type, or null
    //
    // the ValueType-keyed form of AST::find_copy_constructor (ASTMemberLookup.h), which takes a
    // layout: that one owns the template_ref redirect, this one owns the "is there a layout to ask
    // about at all" question, since get_complex_type() asserts and a primitive or a pointer has none
    // both consumers - the gate below and the ownership pass's copy hook - hold a type rather than a
    // layout, so the guard belongs here once instead of at each of them
    FunctionDeclNode *copy_constructor_for(const ValueType &type);

    // does copying a value of this type mean *calling* something, rather than copying its bytes?
    //
    // the copying counterpart of AST::needs_destruction (ASTDestruction.h), and in its own header for
    // the same reason: it is the question, AST::find_copy_constructor (ASTMemberLookup.h) is the
    // lookup. true two ways:
    //
    //  - **the type declares a copy constructor**, so `$b = $a` must mean what `Foo($a)` means. true
    //    whether or not the type owns anything, or which operation you got would depend on whether it
    //    happens to declare a destructor as well
    //  - **the type owns something** (needs_destruction). a byte copy would leave two owners of one
    //    resource and there is nothing sound for it to mean, so the ownership pass reports it
    //
    // a pointer answers false through both arms, so a borrow destination is copied by copying the
    // address - which is what a borrow is
    bool copy_needs_constructor(const ValueType &type);

    // can the compiler write this type's copy constructor itself?
    //
    // the one case book/concept/ownership_and_moving.md leaves to the compiler rather than to the
    // author: a struct whose owning properties are all classes, transitively through struct-typed
    // ones. copying it is a retain per class field, and there is nothing to guess. every *other*
    // owner ends at a raw pointer the type system knows nothing about, so it stays the author's to
    // say - "we cannot copy owners we have no rule for", not "we cannot copy owners"
    //
    // false for three reasons before the properties are even looked at: anything that is not a struct
    // (a class's copy is a retain, and is answered a step earlier); a declared copy constructor, which
    // is the answer already; and a declared **destructor**, because duplicating the value would run
    // that body twice over things its author never said could be duplicated
    //
    // then every property has to have a copy somebody has said how to make - the same question asked
    // of the parts (ASTCopy.cpp's copy_is_defined), which is what makes this recursive. deliberately **not cached** on ComplexType, for the
    // reason AST::needs_destruction gives (ASTDestruction.h). it bottoms out where that one does too:
    // a class property is answered without descending into it
    bool copy_is_synthesizable(const ValueType &type);
};

#endif
