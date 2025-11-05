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

    // **how a value of this type is copied** - the four ways there are, plus the refusal, and the one
    // place that decides between them
    //
    // declaration order is the order they are decided in, and the order is load-bearing: a class is
    // asked about before a declared constructor, because a class's copy is one more reference even
    // when it also declares a `Foo&` constructor - which builds a *new* object, a different
    // operation. see AST::OwnershipPass::arrive_value, which switches on this
    //
    // both readers used to spell the ladder out for themselves - the pass, of the value arriving
    // somewhere, and the synthesis question, of a *part* of that value - so the recursion and the
    // decision it feeds were two implementations held in step by nothing but being written in the
    // same order
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
        // book/concept/ownership_and_moving.md leaves to the compiler rather than to the author
        t_synthesizable,

        // nobody has said what a copy would mean, and the compiler will not guess. the ownership
        // pass's located error
        t_none,
    };

    // **the sole answer to "how is a value of this type copied".** every arm is named above, and both
    // readers switch on it rather than re-deriving it
    //
    // a struct's answer is **folded from its properties' own answers** in one walk, so the taxonomy is
    // compositional: a property with no rule makes the whole struct uncopyable, since it would be
    // copied as bytes alongside the ones that do have a rule and leave two owners of one resource - and
    // a struct whose properties are all byte copies is one itself. this is deliberately *not* asked
    // through AST::needs_destruction (ASTDestruction.h): the two ladders answer different questions - a
    // struct with a destructor owns something and is still not copyable - and asking that one here
    // walked the whole property graph a second time at every level, at every copy site, on every round
    // of the fixpoint
    //
    // deliberately **not cached** on ComplexType, for the reason AST::needs_destruction gives. it
    // bottoms out where that one does too: a class property is answered without descending into it
    CopyKind classify_copy(const ValueType &type);

    // does copying a value of this type mean *calling* something, rather than copying its bytes?
    //
    // the copying counterpart of AST::needs_destruction (ASTDestruction.h). true three ways, and each
    // one is an arm of the classifier: the type is a reference, so a copy is one more of them; **the
    // type declares a copy constructor**, so `$b = $a` must mean what `Foo($a)` means - true whether or
    // not it owns anything, or which operation you got would depend on whether it happens to declare a
    // destructor as well; or the type owns something, whether or not its author has said what a real
    // copy would be, since a byte copy would leave two owners of one resource and there is nothing
    // sound for it to mean
    //
    // a pointer answers false, so a borrow destination is copied by copying the address - which is
    // what a borrow is
    inline bool copy_needs_constructor(const ValueType &type)
    {
        return classify_copy(type) != CopyKind::t_bytes;
    }

    // can the compiler write this type's copy constructor itself?
    //
    // the one case the chapter leaves to the compiler: a struct whose properties each have a copy
    // somebody has said how to make, transitively through struct-typed ones. copying it is a copy per
    // property, and there is nothing to guess. every *other* owner ends at a raw pointer the type
    // system knows nothing about, so it stays the author's to say - "we cannot copy owners we have no
    // rule for", not "we cannot copy owners"
    //
    // three of the classifier's arms answer ahead of this one, and each is a refusal in its own right:
    // anything that is not a struct, since a reference's copy is a retain and nothing else has a layout
    // to ask about; a declared copy constructor, which is the answer already; and a declared
    // **destructor**, because duplicating the value would run that body twice over things its author
    // never said could be duplicated
    inline bool copy_is_synthesizable(const ValueType &type)
    {
        return classify_copy(type) == CopyKind::t_synthesizable;
    }

    // **may a copy of this type read a `const` source?** - which is to say, what does the parameter of
    // the copy constructor AST::OwnershipPass synthesizes have to be, `const Foo&` or `Foo&`
    //
    // asked rather than fixed, because it is not this pass's to decide. a hand-written
    // `constructor(Point& $other)` takes a *mutable* borrow, and by doing so reserves the right to write
    // through it - so a value holding a `Point` cannot promise to copy itself out of a const source
    // however it is spelled here. the const-ness has to be folded out of the properties for the same
    // reason CopyKind is, and this is the same walk with a different question at its leaves
    //
    // the answer is `const` wherever it can be, and that is what matters: it is the difference between
    // `$b = $a` working on a `const Foo&` and being the one operation a const value cannot take part in
    // - including a copy out of a const *place*, `$other[$i]` inside stdlib/core/array.eco's
    // `constructor(const array<T>& $other)`, which is how an owning array copies its elements at all
    //
    // deliberately **not** answered by stripping const at the call instead. a teardown may do that
    // (AST::OwnershipPass::receiver_for_teardown says why: the storage is going away, so nothing that
    // happens to it afterwards is observable) and a copy may **not** - its source outlives it and is
    // read again, so a `Point&` constructor writing through one would be observable
    bool copy_source_may_be_const(const ValueType &type);
};

#endif
