#include "AST/ASTDestruction.h"

#include "AST/ASTMemberLookup.h"

namespace
{
    // "declares a destructor, or holds a property that needs destroying" - the one walk behind both
    // questions below, so a struct's teardown and a class payload's teardown can never disagree about
    // what a body owes
    //
    // asked of the *instantiation's* properties rather than the template's: `Box<Buffer>` needs
    // destruction where `Box<int32>` does not, and only the instantiation's layout has the substituted
    // property types. terminates on a class-typed property, which answers true without descending
    bool body_needs_destruction(const AST::ComplexType *ct)
    {
        if (AST::find_destructor(ct) != nullptr) {
            return true;
        }

        for (size_t i = 0; i < ct->property_count(); i++) {
            if (AST::needs_destruction(ct->get_property_type(i))) {
                return true;
            }
        }

        return false;
    }
}

bool AST::needs_destruction(const AST::ValueType &type)
{
    // a callable owes exactly one release: its captured environment. answered before the leaf below and
    // *without* looking at what it captured, for the same reason a class is - the signature is the type,
    // so two callables of one type may capture different things, and only the value knows which. a
    // non-capturing one carries a null environment and the release is a no-op on it
    if (type.is_callable()) {
        return true;
    }

    // a weak reference owes one weak release. it does **not** own its object - that is the whole point of
    // it - but it does own the block's readability, which is a real obligation with a real count behind it
    //
    // it has to be answered here, ahead of the leaf below: a weak has no ComplexType of its own, so it
    // would otherwise fall out as "owns nothing" and a `weak<T>` field would leak its block forever. and
    // it is answered *without* descending into the class it names, for the reason the class arm below
    // gives - which is also what keeps `class Node { weak<Node> $prev; }` from recursing
    if (type.is_weak()) {
        return true;
    }

    // a primitive owns its own bytes and nothing else. a pointer and a borrow own nothing at all -
    // see the header, this is the leaf of the whole recursion
    if (!type.has_complex_type()) {
        return false;
    }

    // a reference always owes a release, whatever the payload turns out to hold. answered before the
    // property walk below, which is what stops a self-referential class recursing forever
    if (type.is_class()) {
        return true;
    }

    // an interface *value* is a retained class handle, so it owes a release for the same reason a
    // class does and without looking inside for the same reason: the type does not say which object
    // is behind it. beside the class arm rather than after the property walk, because an interface has
    // no properties at all - asking that walk would answer a confident, wrong "owns nothing"
    if (type.is_interface()) {
        return true;
    }

    const AST::ComplexType *ct = type.get_complex_type();

    // a struct that contains an owner is itself an owner
    return ct != nullptr && body_needs_destruction(ct);
}

bool AST::class_needs_deinit(const AST::ComplexType *type)
{
    // exactly the walk needs_destruction does for a struct, asked one level inside the block
    return type != nullptr && type->is_class_kind() && body_needs_destruction(type);
}
