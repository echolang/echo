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
    class VarDeclNode;

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
    // point.
    //
    // An instantiation's conformances are substituted into its own list when it is interned
    // (TypeRegistry::derive_instantiation), because `struct Bag<E> : contract::iterable<E>` conforms to
    // `contract::iterable<int32>` and not to `contract::iterable<E>`.
    //
    // Redirecting to the template instead would hand back a conformance still mentioning `E`, which
    // compares equal to nothing a use site can ask about. Re-substituting at read time would need a
    // TypeRegistry to intern `contract::iterable<int32>` with, which the readers below do not have. So
    // the substitution happens once, where the layout's does
    //
    // a null `ct`, a non-interface `interface`, or an unconstrained type all answer false rather than
    // asserting: every reader is asking a question about types that may not be settled yet
    bool conforms_to(const ComplexType *ct, const ValueType &interface);

    // the same question asked of a type rather than a layout, so a caller with a ValueType in hand does
    // not have to unwrap it and guard the null.
    //
    // a type with a declaration stays **nominal**: `conforms_to` is membership in that layout's
    // conformance list, and a struct that happens to have `<` is not `comparable` until it says so.
    // a type with no declaration - a primitive, a pointer, a simd vector, a callable - has nothing
    // to opt into except the operators the language already spells, so it conforms **structurally**
    // when every requirement is an operator, there are no associated types, and each operator is
    // answered by builtin meaning on the *peeled* operand types. the peel is the load-bearing half:
    // a requirement written `const T&` is a pointer, and asking builtin meaning of the borrow would
    // make every type comparable via the address-comparison arm. the first peeled operand has to
    // equal the type being asked, or `int32` would vacuously conform to `comparable<float64>`
    //
    // the structural arm lives in ASTStructuralConformance.cpp. requirement signatures walk
    // pointers and type parameters without intern: an operator requirement is `const T&`,
    // and first_constraint_violation substitutes a generic application before it asks here
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

    // **the one type argument a single-parameter conformance carries** - `contract::keyed<K>`'s K,
    // `contract::failable<E>`'s E - or nothing.
    //
    // four distinct absences collapse to nullopt and **none of them is a refusal**: the template is
    // unbound (`--no-stdlib`, or a program declaring its own protocol), the type has no complex type,
    // no conformance matches, or the application has the wrong arity. that is what makes it the shape
    // an *optional* side channel wants - AST::iteration_plan_for's key and AST::unwrap_plan_for's
    // failure are both absent-is-fine until a `=>` or an `else ($e)` asks, and each of those refusals
    // belongs at the asker's token rather than here
    std::optional<ValueType> sole_conformance_argument(
        const ValueType &type, const ComplexType *interface_template);

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
    // One owner for that redirect, because it rests on an invariant no caller can see:
    // TypeRegistry::derive_instantiation rebuilds the conformance list in the template's order, so the
    // two lists are index-parallel. Re-derived at a call site, it goes stale silently the day that
    // changes, and the caller then reads its answer off the wrong contract.
    //
    // a `ct` that is not an instantiation answers with itself and an empty substitution, so a caller
    // writes one path. **nullopt is "ask again"**, not a refusal: it means the lists could not be lined
    // up, which for a half-derived instantiation is a state a later round leaves behind
    std::optional<TemplateConformance> template_conformance_for(ComplexType *ct, const ValueType &applied);

    // what a conformance binds: the interface's own parameters and its associated types, derived on
    // every ask (nothing is stored). unify proposes; `==` decides. drop non-associated bindings so
    // an ill-fitting candidate cannot rebind the interface's own `V`
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

    // first unanswered requirement, in declaration order. skip the receiver (`Drawable&` vs
    // `Circle&`). operators live in the root namespace, so `functions` is asked; null skips them.
    // `types` is the bundle's own registry: a second one would intern a second ComplexType
    std::optional<UnmetRequirement> first_unmet_requirement(
        const ComplexType *ct,
        const ValueType &interface,
        TypeRegistry &types,
        const FunctionRegistry *functions);

    // **which template methods a vtable for `ct` as `interface` would name**, aligned index-for-index
    // with interface_requirements(). empty when any requirement is unanswered. the candidate is always
    // the *template* find_member_functions returns - mapping it to an instance is
    // interface_implementations' job after the monomorphizer has force-emitted it
    //
    // one walk so instantiate_interface_methods and the vtable cannot disagree about which bodies the
    // table holds. an entry is null only for an **operator** requirement, which has no receiver and
    // therefore no slot
    std::vector<FunctionDeclNode *> interface_implementation_templates(
        const ComplexType *ct,
        const ValueType &interface,
        TypeRegistry &types
    );

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

    // the interface whose default has `receiver` as its `$this`, or empty
    std::string default_interface_name(const VarDeclNode *receiver);

    // the instance args a slot's body needs: the class's for a method, [conformance bindings..., ct] for a default, empty for none
    std::vector<ValueType> implementation_instance_args(
        const FunctionDeclNode *answer,
        const ComplexType *ct,
        const ValueType &interface,
        TypeRegistry &types
    );

    // a vtable slot whose instance get_or_create_function_instance has not minted yet
    struct UninstantiatedImplementation
    {
        FunctionDeclNode *tmpl = nullptr;
        std::vector<ValueType> args;
    };

    // each slot of `ct`'s tables that still needs an instance. a non-generic class's own method is
    // absent (empty args); an already-minted default is absent
    std::vector<UninstantiatedImplementation> uninstantiated_implementations(
        ComplexType *ct,
        TypeRegistry &types
    );

    // **why a value of `from` cannot be *stored* as `interface`**, or an empty string when it can.
    //
    // deliberately a different question from conforms_to, which is about the contract. this one is about
    // whether an erased value can be built and dispatched at all, and the answers are the holes the
    // storable half of the feature has: a struct or enum, an associated type, an operator requirement.
    // a generic class instantiation is not one of them - its requirement methods are force-emitted in
    // the monomorphizer's fixpoint, and the vtable names those instances. every refusal is a diagnostic
    // the type checker owes:
    // reaching codegen without it is an internal error, because coerce_value has no table to fall back on
    //
    // the message is a whole sentence, phrased for the author of the *widening* - they are the one who has
    // to change something, and the reason is never obvious from the types alone
    std::string interface_erasure_refusal(const ValueType &from, const ValueType &interface);

    // **why this interface cannot be a stored value**, regardless of what is being erased.
    // associated types and operator requirements. the dest half of interface_erasure_refusal,
    // asked on its own by a recast whose source is already an interface
    std::string interface_storage_refusal(const ValueType &interface);
};

#endif
