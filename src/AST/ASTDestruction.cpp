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

    const AST::ComplexType *ct = type.get_complex_type();

    // a struct that contains an owner is itself an owner
    return ct != nullptr && body_needs_destruction(ct);
}

bool AST::class_needs_deinit(const AST::ComplexType *type)
{
    // exactly the walk needs_destruction does for a struct, asked one level inside the block
    return type != nullptr && type->is_class_kind() && body_needs_destruction(type);
}
