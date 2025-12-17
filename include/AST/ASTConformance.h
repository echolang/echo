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
    // (TypeRegistry::derive_instantiation), because `struct Bag<E> : contract::iterable<E>` conforms to
    // `contract::iterable<int32>` and not to `contract::iterable<E>`. redirecting to the template instead
    // would hand back a conformance still mentioning `E`, which compares equal to nothing a use site can ask
    // about - and re-substituting at read time would need a TypeRegistry to intern
    // `contract::iterable<int32>` with, which the readers below do not have. so the substitution happens
    // once, where the layout's does
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

    // the associated types `interface` declares - `type Iter : contract::iterator<V>` - in declaration order.
    // empty for anything that is not an interface, and through the same `template_or_self` redirect
    // interface_requirements takes, for the same reason: `contract::iterable<int32>` is an interned ComplexType
    // that declares nothing of its own
    const std::vector<TypeParamDecl *> &interface_associated_types(const ComplexType *interface);

    // **which of `ct`'s conformances apply `interface_template`** - which `contract::iterable<...>` an
    // `array<int32>` conforms to.
    //
    // the question conforms_to cannot answer, and deliberately so: it takes the *applied* interface,
    // because a use site always knows what it is asking about. this caller is the one trying to find
    // out, so it has only the template
    //
    // a vector, because the conformance list is a set of *applications* and not of templates:
    // `: Comparable<int32>, Comparable<float64>` is two legitimate entries over one template, so "the"
    // conformance is a question with two answers and the caller is the one who has to word that
    std::vector<ValueType> conformances_matching_template(
        const ComplexType *ct, const ComplexType *interface_template);

    // the single-answer form. nullopt when none matches **and** when more than one does - a caller that
    // needs to tell those apart asks the vector form above
    std::optional<ValueType> conformance_matching_template(
        const ComplexType *ct, const ComplexType *interface_template);

    // a conformance as the *template* spells it, and the way back to the instance
    struct TemplateConformance
    {
        // the declaration the conformance work is done on: the template for an instantiation, and the
        // type itself for anything else
        ComplexType *owner = nullptr;

        // `owner`'s own spelling of the conformance asked about
        ValueType conformance;

        // takes an answer read off `owner` back to the instance. empty when there was no redirect
        TypeSubstitution to_instance;
    };

    // **an instantiation's conformance work belongs on its template, and the answer is substituted
    // back.** AST::find_member_functions redirects an instantiation through `template_ref`, so a member
    // of `array<int32>` is `array<T>`'s declaration returning `slice_iterator<T>` - while a signature
    // substituted from the *applied* conformance reads `int32`. comparing those two directly never
    // matches, which is why AST::TypeChecker checks a generic conformance once on the template, where
    // both sides share the same TypeParamDecl.
    //
    // one owner for that redirect, because it rests on an invariant no caller can see:
    // TypeRegistry::derive_instantiation rebuilds the conformance list in the template's order, so the
    // two lists are index-parallel. re-derived at a call site it goes stale silently the day that
    // changes, and the caller then reads its answer off the wrong contract.
    //
    // a `ct` that is not an instantiation answers with itself and an empty substitution, so a caller
    // writes one path. **nullopt is "ask again"**, not a refusal: it means the lists could not be lined
    // up, which for a half-derived instantiation is a state a later round leaves behind
    std::optional<TemplateConformance> template_conformance_for(ComplexType *ct, const ValueType &applied);

    // **what a conformance binds**: the interface's own type parameters, from the arguments the clause
    // spelled, *and* its associated types, inferred from the members that answer the requirements
    // mentioning them.
    //
    // one solve, because first_unmet_requirement and interface_implementations must read a requirement's
    // signature through the same substitution - this header already promises the two cannot disagree
    // about whether a conformance is met, and this is now half of that answer.
    //
    // **nothing is stored.** an inferred binding is knowable only once the implementor's methods exist,
    // which is strictly later than TypeRegistry::get_or_create_instantiation's staleness test can notice
    // (todo/A7) - so it is derived on every ask, and an instantiation's answer is its template's
    // substituted through instantiation_args. that read-time redirect is the one conforms_to declines
    // only because *its* readers have no TypeRegistry, which this one is handed
    //
    // the inference itself: unify each requirement's wanted signature against the members that could
    // answer it, keep **only** the associated bindings out of the result, and then re-check with the
    // existing exact `==`. unify_type only proposes; ValueType::operator== still decides. dropping the
    // non-associated bindings is what stops unification from quietly rebinding the interface's own `V`
    // to whatever an ill-fitting candidate happened to declare
    struct ConformanceBinding
    {
        enum class Failure
        {
            t_none,

            // two members proposed different types for one associated name
            t_ambiguous,

            // inferred, but the associated type's own constraint - substituted through this very
            // conformance - rejects it
            t_constraint,
        };

        // `V -> int32` *and* `Iter -> slice_iterator<int32>`. complete only when `failure` is t_none
        TypeSubstitution substitution;

        Failure failure = Failure::t_none;

        // the associated type the failure is about, null when there is none
        const TypeParamDecl *associated = nullptr;

        // t_ambiguous: the two proposals. t_constraint: `first` is what it was bound to
        ValueType first;
        ValueType second;

        // t_constraint only: the associated type's constraint **substituted through this conformance**,
        // e.g. `contract::iterator<int32>` rather than the `contract::iterator<V>`
        // TypeParamDecl::constraint_spelling holds. carried rather than re-derived by the reporter, which has
        // no substitution of its own - and naming the interface's own `V` at an implementor is the thing
        // WantedSignature already avoids
        std::string constraint_spelling;
    };

    ConformanceBinding conformance_bindings(
        const ComplexType *ct, const ValueType &interface, TypeRegistry &types);

    // why a conformance is not satisfied, or nullopt when it is. the *declaration site's* question, and
    // the only thing that turns a declared conformance into a checked one
    struct UnmetRequirement
    {
        // the requirement `ct` does not answer. never null when this is returned
        const FunctionDeclNode *requirement = nullptr;

        // the requirement's signature with the interface's own type parameters substituted through the
        // conformance being checked - `contract::iterable<int32>`'s `next()` reads `ptr<int32>`, not `ptr<T>`.
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
    // `array<T>` into `array<int32>` re-interns, and interning through a second registry would mint a
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
        TypeRegistry &types
    );

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
