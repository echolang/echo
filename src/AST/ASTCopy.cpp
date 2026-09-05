#include "AST/ASTCopy.h"

#include "AST/ASTCompleteness.h"
#include "AST/ASTMemberLookup.h"
#include "AST/FunctionDeclNode.h"

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
                case AST::CopyKind::t_elements:
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

    // **`#[unique]` is the one refusal that outranks a declared constructor**, and the order is the
    // content: the whole claim is that exactly one value names this storage, so a copy constructor
    // written on such a type is a contradiction rather than an answer to it. every other arm below
    // decides *how* to copy; this one decides that there is no such thing
    //
    // asked of *any* type that can carry the flag rather than of a struct, because a class and an
    // interface are refused it at the declaration and have already returned above - so the kind test
    // added nothing and would have had to grow an arm for every kind that can be written `#[unique]`.
    // an enum can, and means exactly the same thing by it
    if (type.has_complex_type() && type.get_complex_type()->is_unique) {
        return AST::CopyKind::t_none;
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
    //
    // **an enum falls through with a struct and that is the whole argument for its layout being flat.**
    // its properties are `__tag` and one slot per payload field, so the fold below answers over the
    // payloads with no arm here and no arm in AST::needs_destruction: an enum any of whose cases owns
    // something is t_synthesizable, and one whose cases own nothing copies as bytes. a union buffer
    // would arrive here as a `[N x i8]` property and fold to t_bytes - silently, and for exactly the
    // shape that must not
    if (type.is_inline_array()) {
        switch (AST::classify_copy(type.array_element())) {
        case AST::CopyKind::t_bytes:
            return AST::CopyKind::t_bytes;

        case AST::CopyKind::t_none:
            return AST::CopyKind::t_none;

        case AST::CopyKind::t_retain:
        case AST::CopyKind::t_constructor:
        case AST::CopyKind::t_synthesizable:
        case AST::CopyKind::t_elements:
            return AST::CopyKind::t_elements;
        }
    }

    // an incomplete type has no value, so it has no copy. TypeChecker refused the declaration
    // that would have asked; this is the backstop so a slipped-through one is not a byte copy
    // of a type with no size. AST::type_completeness is the owner, not is_opaque(): an array
    // or tagged optional of an incomplete payload is incomplete too
    if (AST::type_completeness(type) == AST::TypeCompleteness::t_incomplete) {
        return AST::CopyKind::t_none;
    }

    if (!type.is_struct() && !type.is_enum()) {
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

bool AST::copy_source_may_be_const(const AST::ValueType &type)
{
    switch (AST::classify_copy(type)) {
        // nothing is called, so there is nothing to be refused by. bytes are read wherever they sit, and
        // a retain writes a count inside the block rather than through the handle it was handed
        case AST::CopyKind::t_bytes:
        case AST::CopyKind::t_retain:
            return true;

        // **the author's declaration is the answer.** `constructor(const Point& $other)` promises to read
        // its source and `constructor(Point& $other)` does not, and the second one is a legitimate thing
        // to write - so what it says is what holds, here and transitively for everything holding one
        case AST::CopyKind::t_constructor:
        {
            const AST::FunctionDeclNode *ctor = AST::copy_constructor_for(type);

            return ctor != nullptr && ctor->parameter_type(0).pointee().is_const();
        }

        // the body the compiler writes is a copy per property, so it can read a const source exactly
        // when every one of those copies can. the same fold classify_copy just did, asked one question
        // further along - and it terminates where that one does, since a class property answers above
        // without being descended into
        case AST::CopyKind::t_synthesizable:
        {
            const AST::ComplexType *ct = type.get_complex_type();

            for (size_t i = 0; i < ct->property_count(); i++) {
                if (!AST::copy_source_may_be_const(ct->get_property_type(i))) {
                    return false;
                }
            }

            return true;
        }

        // there is no copy to give a parameter to. answered so the switch stays total rather than because
        // anything reads it - AST::OwnershipPass reports this arm and never reaches a synthesis
        case AST::CopyKind::t_none:
            return true;

        // the loop copies each element the way that element is copied, so a const source is
        // readable exactly when T is
        case AST::CopyKind::t_elements:
            return AST::copy_source_may_be_const(type.array_element());
    }

    return true;
}
