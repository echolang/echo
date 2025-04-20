#include "AST/ASTCopy.h"

#include "AST/ASTDestruction.h"
#include "AST/ASTMemberLookup.h"

AST::FunctionDeclNode *AST::copy_constructor_for(const AST::ValueType &type)
{
    if (!type.has_complex_type()) {
        return nullptr;
    }

    return AST::find_copy_constructor(type.get_complex_type());
}

bool AST::copy_needs_constructor(const AST::ValueType &type)
{
    // the declared answer first, and deliberately not gated on ownership: a type that says how it is
    // copied is copied that way, so the explicit `Foo($a)` and the implicit `$b = $a` cannot diverge
    if (AST::copy_constructor_for(type) != nullptr) {
        return true;
    }

    // and the one the compiler asks on its own behalf: an owning value has no byte copy, whether or
    // not its author has said what a real one would be
    return AST::needs_destruction(type);
}

AST::CopyKind AST::classify_copy(const AST::ValueType &type)
{
    // the byte copy first, through the shared gate rather than through its two halves: a type that
    // owns nothing *and* has said nothing about copying is copied the way it always was
    if (!AST::copy_needs_constructor(type)) {
        return AST::CopyKind::t_bytes;
    }

    // ahead of the declared constructor - see the header for why that order is not free
    if (type.is_class()) {
        return AST::CopyKind::t_retain;
    }

    // a callable is copied the way a class is: the copy shares the captured environment, so it is one
    // more reference to it. a callable can declare nothing, so there is no constructor arm to order
    // against - but it has to precede the two below, which both go looking for properties it has none of
    if (type.is_callable()) {
        return AST::CopyKind::t_retain;
    }

    // an interface value is a class handle wearing an erased type, so it is copied the way a class is
    // and for the same reason a callable is: what is inside is a different question, and the type cannot
    // answer it. beside these two rather than below, because an interface declares no copy constructor
    // (refused at its declaration) and has no properties for either arm below to walk - asking them
    // would answer t_none, which reads to the author as "this type cannot be copied at all"
    if (type.is_interface()) {
        return AST::CopyKind::t_retain;
    }

    // and a weak reference, which needed **no new kind**: `t_retain` says "one more reference to the same
    // object", and one more weak reference is exactly that. which count moves is read off the ValueType at
    // the two sites that emit the code (ClassCodegen::gen_retain_value / gen_release_value), so the copy
    // taxonomy stays four ways of copying rather than five
    //
    // that also means the `releases_old` gate in OwnershipPass, which asks `classify_copy(...) ==
    // t_retain`, admits a weak assignment target with nothing added: `$node->prev = &$other` gives back
    // the weak reference it was holding, in the order gen_assign already fixes
    if (type.is_weak()) {
        return AST::CopyKind::t_retain;
    }

    if (AST::copy_constructor_for(type) != nullptr) {
        return AST::CopyKind::t_constructor;
    }

    // the recursion into properties lives here
    if (AST::copy_is_synthesizable(type)) {
        return AST::CopyKind::t_synthesizable;
    }

    return AST::CopyKind::t_none;
}

namespace
{
    // "every property has a copy somebody has said how to make". the copying counterpart of
    // ASTDestruction.cpp's body_needs_destruction, and the same shape: the walk lives here, the
    // public entry below owns the guards
    bool properties_are_copyable(const AST::ComplexType *ct)
    {
        for (size_t i = 0; i < ct->property_count(); i++) {
            // one uncopyable property is enough: it would be copied as bytes alongside the ones that
            // do have a rule, leaving two owners of one resource
            //
            // asked through the same classifier the ownership pass dispatches on, so "has somebody
            // said how this is copied" cannot come apart from "what does copying it do"
            if (AST::classify_copy(ct->get_property_type(i)) == AST::CopyKind::t_none) {
                return false;
            }
        }

        return true;
    }
};

bool AST::copy_is_synthesizable(const AST::ValueType &type)
{
    // a class is copied by retaining it, which resolve_value_arrival answers before it gets here, and
    // everything else has no layout to ask about - so this is has_complex_type() minus the classes
    if (!type.is_struct()) {
        return false;
    }

    const AST::ComplexType *ct = type.get_complex_type();

    // a written copy constructor is the answer, and is never replaced by a synthesized one
    if (AST::find_copy_constructor(ct) != nullptr) {
        return false;
    }

    // a destructor is the author saying this value's teardown is theirs. what a second value
    // running that same body would mean is exactly the question they have not answered - and the
    // one place it is knowable is the type itself, which is why the compiler does not guess
    if (AST::find_destructor(ct) != nullptr) {
        return false;
    }

    return properties_are_copyable(ct);
}
