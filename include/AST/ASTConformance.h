#ifndef ASTCONFORMANCE_H
#define ASTCONFORMANCE_H

#pragma once

#include "AST/ASTValueType.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class FunctionDeclNode;
    class FunctionRegistry;

    // **two questions, deliberately two functions.** does a type *claim* an interface, and does it
    // *deliver* it. the first is what every use site reads - a type-parameter constraint, an
    // `instanceof`, a widening - and it has to be cheap and total. the second is what the declaration
    // site reports on, and it is the only thing that makes the first one's answer mean anything.
    //
    // splitting them is what keeps conformance *declared*: a use site never re-derives whether the
    // members line up, so a diagnostic about a missing method fires once, at the implementor, rather
    // than at every call that happened to need it.

    // does `ct` declare that it conforms to `interface`? a membership test over the type's own
    // conformance list, and nothing more.
    //
    // **no template_ref redirect here, unlike every lookup in ASTMemberLookup.h** - and that is the
    // point. an instantiation's conformances are substituted into its own list when it is interned
    // (TypeRegistry::derive_instantiation), because `struct Bag<E> : Iterable<E>` conforms to
    // `Iterable<int32>` and not to `Iterable<E>`. redirecting to the template instead would hand back
    // a conformance still mentioning `E`, which compares equal to nothing a use site can ask about -
    // and re-substituting at read time would need a TypeRegistry to intern `Iterable<int32>` with,
    // which the readers below do not have. so the substitution happens once, where the layout's does
    //
    // a null `ct`, a non-interface `interface`, or an unconstrained type all answer false rather than
    // asserting: every reader is asking a question about types that may not be settled yet
    bool conforms_to(const ComplexType *ct, const ValueType &interface);

    // the same question asked of a type rather than a layout, so a caller with a ValueType in hand does
    // not have to unwrap it and guard the null. a primitive, a pointer or a callable conforms to nothing
    bool conforms_to(const ValueType &type, const ValueType &interface);

    // the requirements `interface` declares, in **declaration order** - which is also vtable slot order,
    // so this is the one walk that decides both. an empty list for anything that is not an interface
    //
    // the interface's own type parameters are *not* substituted here: a caller comparing against an
    // implementor needs the substitution built from the conformance it is checking, which
    // requirement_signature_for below is given
    //
    // by reference, because two of its readers are on repeated paths - interface_method_slot runs once
    // per virtual call site and interface_erasure_refusal once per erased arrival - and the list is the
    // owner's own `_methods`, which outlives every caller
    const std::vector<FunctionDeclNode *> &interface_requirements(const ComplexType *interface);

    // the vtable slot `requirement` occupies on `interface`, or nullopt when it is not one of its
    // requirements. the ordinal in interface_requirements() above - so the two can never disagree about
    // which slot a method is in, which is the one thing a vtable cannot get wrong
    std::optional<size_t> interface_method_slot(const ComplexType *interface, const FunctionDeclNode *requirement);

    // why a conformance is not satisfied, or nullopt when it is. the *declaration site's* question, and
    // the only thing that turns a declared conformance into a checked one
    struct UnmetRequirement
    {
        // the requirement `ct` does not answer. never null when this is returned
        const FunctionDeclNode *requirement = nullptr;

        // the requirement's signature with the interface's own type parameters substituted through the
        // conformance being checked - `Iterable<int32>`'s `next()` reads `ptr<int32>`, not `ptr<T>`.
        // what the diagnostic must render, because the unsubstituted form names a parameter the author
        // of the *implementor* never wrote
        std::string wanted_signature;

        // set when a member of the right name exists but not of the right shape, so the diagnostic can
        // say "close, but" rather than "missing". empty when nothing of that name was found at all
        std::string found_signature;
    };

    // the first requirement of `interface` that `ct` does not satisfy, in declaration order.
    //
    // a candidate matches when its parameters **from index 1** and its return type equal the
    // requirement's, after substitution. **index 1, not 0** - argument 0 is the receiver, and it is
    // `Drawable&` on the requirement and `Circle&` on the implementor by construction, so comparing it
    // would make every conformance fail. that is the one asymmetry in the comparison and it is the same
    // one FunctionDeclNode::implicit_arg_count() already exists for
    //
    // an **operator** requirement is not looked up on the type at all: an operator is registered in the
    // root namespace under a decorated name, never on either operand's method table, so `functions` is
    // asked instead. passing null declines to check operator requirements, which is what a caller with
    // no registry to hand (a unit test asking only about methods) wants
    //
    // `types` is the bundle's own registry, and it has to be that one: substituting a requirement's
    // `Array<T>` into `Array<int32>` re-interns, and interning through a second registry would mint a
    // second ComplexType for one type - which would then compare unequal to the implementor's, since
    // ValueType equality *is* pointer identity. every such lookup here is a cache hit, because the
    // interface and the conformance clause both interned what they mention when they were parsed
    std::optional<UnmetRequirement> first_unmet_requirement(
        const ComplexType *ct,
        const ValueType &interface,
        TypeRegistry &types,
        const FunctionRegistry *functions);

    // **which declaration answers each requirement**, aligned index-for-index with
    // interface_requirements() - so this *is* the vtable, in slot order, and the slot a dispatch site
    // reads is the slot this walk filled. empty when any requirement is unanswered, which is exactly
    // when first_unmet_requirement returns a value: one matcher, asked from the two directions its two
    // readers need, so a conformance can never be reported met and lowered wrong
    //
    // an entry is null only for an **operator** requirement, which has no receiver and therefore no
    // slot to dispatch through - the reason an interface declaring one cannot be a storable type
    //
    // no FunctionRegistry, unlike first_unmet_requirement: an operator requirement is the one thing this
    // walk does not look up, because it fills a *slot* and an operator has none
    std::vector<FunctionDeclNode *> interface_implementations(
        const ComplexType *ct,
        const ValueType &interface,
        TypeRegistry &types);

    // **why a value of `from` cannot be *stored* as `interface`**, or an empty string when it can.
    //
    // deliberately a different question from conforms_to, which is about the contract. this one is about
    // whether an erased value can be built and dispatched at all, and the three answers are the three
    // holes the storable half of the feature has. every one of them is a diagnostic the type checker owes:
    // reaching codegen without it is an internal error, because coerce_value has no table to fall back on
    //
    // the message is a whole sentence, phrased for the author of the *widening* - they are the one who has
    // to change something, and the reason is never obvious from the types alone
    std::string interface_erasure_refusal(const ValueType &from, const ValueType &interface);
};

#endif
