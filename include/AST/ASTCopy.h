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
    // both consumers - the classifier below and the ownership pass's copy hook - hold a type rather
    // than a layout, so the guard belongs here once instead of at each of them
    FunctionDeclNode *copy_constructor_for(const ValueType &type);

    // **how a value of this type is copied** - the five ways there are, plus the refusal, and the one
    // place that decides between them
    //
    // declaration order is the order they are decided in, and the order is load-bearing: a class is
    // asked about before a declared constructor, because a class's copy is one more reference even
    // when it also declares a `Foo&` constructor - which builds a *new* object, a different
    // operation. see AST::OwnershipPass::arrive_value, which switches on this
    //
    // both readers switch on this rather than spelling the ladder out for themselves - the pass,
    // of the value arriving somewhere, and the synthesis question, of a *part* of that value - so
    // the recursion and the decision it feeds cannot drift
    enum class CopyKind
    {
        // nothing to arrange: a primitive, a pointer, or a struct whose properties are all this. copied
        // as bytes, the way every copy in the language worked before ownership existed
        t_bytes,

        // one more reference to the same object. the bottom of the recursion, exactly as it is for
        // destruction - what the class *holds* is a different question, and nobody asks it here
        t_retain,

        // the constructor its author wrote, recognised rather than newly spelled - so the explicit
        // `Foo($a)` and the implicit `$b = $a` are the same declaration
        t_constructor,

        // a body the compiler can write itself: a struct whose properties each have an answer of their
        // own, which is copied by copying each of them. the one case
        // left to the compiler rather than to the author
        t_synthesizable,

        // nobody has said what a copy would mean, and the compiler will not guess. the ownership
        // pass's located error
        t_none,

        // an inline array whose element is copyable by something other than bytes: repeat that
        // element's copy N times. not a constructor - `T[N]` has no ComplexType to hang one on -
        // and not synthesizable, which means "write a constructor on this layout"
        t_elements,
    };

    // how a value of this type is copied. a struct folds from its properties: one uncopyable field
    // makes the whole uncopyable. a different ladder from needs_destruction, and not cached on ComplexType
    CopyKind classify_copy(const ValueType &type);

    // true when a copy is a call rather than bytes: a retain, a declared constructor, or an owner
    inline bool copy_needs_constructor(const ValueType &type)
    {
        // everything that is not a byte copy goes through arrive_value's constructor switch,
        // including t_none (so the refusal is that switch's) and t_elements (so the loop is)
        return classify_copy(type) != CopyKind::t_bytes;
    }

    // a struct whose properties each have a known copy, transitively. a declared destructor is not
    inline bool copy_is_synthesizable(const ValueType &type)
    {
        return classify_copy(type) == CopyKind::t_synthesizable;
    }

    // may a copy read a `const` source? folded from the properties, because a hand-written
    // `constructor(Point& $other)` may write through. a teardown may strip const; a copy may not
    bool copy_source_may_be_const(const ValueType &type);
};

#endif
