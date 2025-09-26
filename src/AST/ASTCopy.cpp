#include "AST/ASTCopy.h"

#include "AST/ASTMemberLookup.h"

AST::FunctionDeclNode *AST::copy_constructor_for(const AST::ValueType &type)
{
    if (!type.has_complex_type()) {
        return nullptr;
    }

    return AST::find_copy_constructor(type.get_complex_type());
}

namespace
{
    // **the one walk**: a struct is copied by copying each of its properties, so its answer is the fold
    // of theirs. the copying counterpart of ASTDestruction.cpp's body_needs_destruction, and the same
    // shape - the walk lives here, the classifier below owns the guards that decide whether to reach it
    //
    // asked through the same classifier the ownership pass switches on, so "has somebody said how this
    // is copied" cannot come apart from "what does copying it do"
    AST::CopyKind fold_property_copies(const AST::ComplexType *ct)
    {
        bool anything_to_arrange = false;

        for (size_t i = 0; i < ct->property_count(); i++) {
            switch (AST::classify_copy(ct->get_property_type(i))) {
                // one property with no rule is enough: it would be copied as bytes alongside the ones
                // that do have one, leaving two owners of one resource
                case AST::CopyKind::t_none:
                    return AST::CopyKind::t_none;

                // a property that copies as bytes says nothing about the struct holding it - a struct
                // of nothing but these is a byte copy itself, which is what the flag below tracks
                case AST::CopyKind::t_bytes:
                    break;

                // a retain, a declared constructor or a synthesizable property: each is a copy the
                // synthesized body's field-wise assignment reaches on the next round, through the very
                // arm that put it there
                case AST::CopyKind::t_retain:
                case AST::CopyKind::t_constructor:
                case AST::CopyKind::t_synthesizable:
                    anything_to_arrange = true;
                    break;
            }
        }

        return anything_to_arrange ? AST::CopyKind::t_synthesizable : AST::CopyKind::t_bytes;
    }
};

AST::CopyKind AST::classify_copy(const AST::ValueType &type)
{
    // the reference kinds, and they are one arm because they are one answer: a copy is one more
    // reference, and what is behind it is a different question the type cannot answer
    //
    // ahead of the declared constructor below - see the header for why that order is not free. a
    // callable shares its captured environment, an interface value is a class handle wearing an erased
    // type, and a weak reference needed **no new kind** of its own: which count moves is read off the
    // ValueType at the two sites that emit the code (ClassCodegen::gen_retain_value /
    // gen_release_value), so the taxonomy stays four ways of copying rather than five
    //
    // that also means the `releases_old` gate in OwnershipPass, which asks for t_retain, admits a weak
    // assignment target with nothing added: `$node->prev = &$other` gives back the weak reference it
    // was holding, in the order gen_assign already fixes
    if (type.is_class() || type.is_callable() || type.is_interface() || type.is_weak()) {
        return AST::CopyKind::t_retain;
    }

    // the declared answer, deliberately not gated on ownership: a type that says how it is copied is
    // copied that way, so the explicit `Foo($a)` and the implicit `$b = $a` cannot diverge. and ahead
    // of the destructor arm below, so a type declaring both is copied the way it says
    if (AST::copy_constructor_for(type) != nullptr) {
        return AST::CopyKind::t_constructor;
    }

    // nothing else has a layout to ask about, so nothing else can own anything the compiler can see: a
    // primitive owns its own bytes, a pointer and a borrow own nothing at all, and a type parameter the
    // fixpoint has not settled yet must read as "nothing to arrange" rather than as a refusal - it is a
    // not-yet, and the instantiation is classified on its own
    if (!type.is_struct()) {
        return AST::CopyKind::t_bytes;
    }

    const AST::ComplexType *ct = type.get_complex_type();

    // a destructor is the author saying this value's teardown is theirs. what a second value running
    // that same body would mean is exactly the question they have not answered - and the one place it is
    // knowable is the type itself, which is why the compiler does not guess
    if (AST::find_destructor(ct) != nullptr) {
        return AST::CopyKind::t_none;
    }

    return fold_property_copies(ct);
}
