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

namespace
{
    // has *somebody* said how a value of this type is copied? the four ways there are, in one place:
    // its author's constructor, one more reference to a class, a body the compiler can write itself,
    // or nothing to arrange because the value owns nothing
    //
    // the same four arms AST::OwnershipPass::resolve_value_arrival decides between - this is them
    // asked of a *part* rather than of the value being copied, which is what makes the synthesis
    // question recursive. it is not the pass's gate, so the two are held in step by nothing but the
    // arms being listed in the same order in both
    bool copy_is_defined(const AST::ValueType &type)
    {
        // the constructor its author wrote. asked first so it is looked up once for both arms below -
        // and it is the answer whether or not the type owns anything, exactly as
        // copy_needs_constructor says
        if (AST::copy_constructor_for(type) != nullptr) {
            return true;
        }

        // nothing to arrange: a primitive, a pointer, or a struct that owns nothing. copied as bytes,
        // the way every copy in the language worked before this pass existed
        if (!AST::needs_destruction(type)) {
            return true;
        }

        // one more reference to the same object. the bottom of the recursion, exactly as it is for
        // destruction - what the class *holds* is a different question, and nobody asks it here
        if (type.is_class()) {
            return true;
        }

        // or the body the compiler writes itself, which is where the recursion into properties lives
        return AST::copy_is_synthesizable(type);
    }

    // "every property has a copy somebody has said how to make". the copying counterpart of
    // ASTDestruction.cpp's body_needs_destruction, and the same shape: the walk lives here, the
    // public entry below owns the guards
    bool properties_are_copyable(const AST::ComplexType *ct)
    {
        for (size_t i = 0; i < ct->property_count(); i++) {
            // one uncopyable property is enough: it would be copied as bytes alongside the ones that
            // do have a rule, leaving two owners of one resource
            if (!copy_is_defined(ct->get_property_type(i))) {
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
