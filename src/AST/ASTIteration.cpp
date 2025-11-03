#include "AST/ASTIteration.h"

#include "AST/ASTConformance.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTMemberLookup.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

namespace
{
    AST::IterationLookup refuse(std::string reason)
    {
        AST::IterationLookup lookup;
        lookup.result = AST::IterationLookup::Result::t_refused;
        lookup.refusal = std::move(reason);
        return lookup;
    }

    AST::IterationLookup pending()
    {
        return AST::IterationLookup {};
    }

    // `contract::keyed<K>` on the cursor, if it declares one. absent is not an error here - only a `=>`
    // asking for it makes it one, and that refusal belongs at the `=>`
    std::optional<AST::ValueType> key_type_of(const AST::ValueType &iterator, const AST::CoreTypes &core)
    {
        const AST::ComplexType *keyed = core.declared_template(AST::CoreTypeKind::t_keyed);

        if (keyed == nullptr || !iterator.has_complex_type()) {
            return std::nullopt;
        }

        const auto conformance =
            AST::conformance_matching_template(iterator.get_complex_type(), keyed);

        if (!conformance.has_value()) {
            return std::nullopt;
        }

        const AST::ComplexType *applied = conformance->get_complex_type();

        if (applied == nullptr || applied->instantiation_args.size() != 1) {
            return std::nullopt;
        }

        return applied->instantiation_args[0];
    }
}

AST::IterationLookup AST::iteration_plan_for(
    const AST::ValueType &source, const AST::CoreTypes &core, AST::TypeRegistry &types)
{
    // **the unbound case comes first.** `--no-stdlib` is a legitimate program, and this is also what
    // keeps stdlib/core/contract.eco itself compilable - it is parsed by the very compiler that would
    // otherwise assert on the interfaces not existing yet
    const AST::ComplexType *iterator_tmpl = core.declared_template(AST::CoreTypeKind::t_iterator);
    const AST::ComplexType *iterable_tmpl = core.declared_template(AST::CoreTypeKind::t_iterable);

    if (iterator_tmpl == nullptr || iterable_tmpl == nullptr) {
        return refuse(
            "'foreach' needs the iteration protocol, and nothing in this program declares "
            "'#[core: \"iterator\"]'. it lives in the standard library, which this compilation left out.");
    }

    // not settled yet - a type parameter awaiting substitution, or a call that has not resolved. asking
    // again next round is the answer, and it must not read as a refusal
    if (AST::is_undetermined_type(source)) {
        return pending();
    }

    // the two interfaces as the stdlib spells them. every refusal below quotes these rather than a
    // literal, so moving them - into `contract::`, say - moves what the diagnostics tell the user to write
    const std::string iterator_name = core.spelling(AST::CoreTypeKind::t_iterator);
    const std::string iterable_name = core.spelling(AST::CoreTypeKind::t_iterable);

    // the loop reads *through* a borrow exactly as every other reader does; `foreach ($a as ...)` over a
    // `array<int32>&` parameter is the ordinary case, not a special one
    const AST::ValueType subject = AST::ValueType::make_mutable(AST::target_type_of(source));

    if (!subject.has_complex_type()) {
        return refuse(fmt::format(
            "'{}' cannot be iterated - it declares neither '{}' nor '{}'.",
            source.get_type_desciption(), iterator_name, iterable_name));
    }

    AST::ComplexType *subject_ct = subject.get_complex_type();

    AST::IterationLookup lookup;
    lookup.result = AST::IterationLookup::Result::t_ok;

    // **(b) and (c) are one plan reached two ways.** the cursor is the subject either way, and all that
    // differs is which ComplexType carries the `contract::iterator<V>` application V is read off - the erased
    // value's own, or the one its conformance spells. how the two calls dispatch is codegen's business
    // and not this plan's, so the tail below is shared
    const AST::ComplexType *cursor = nullptr;

    // (c) an erased interface value. `contract::iterator<int32>` stored as itself: drive it through the
    // vtable
    if (subject.is_interface()) {
        if (subject_ct->template_or_self() == iterable_tmpl->template_or_self()) {
            return refuse(fmt::format(
                "an erased '{}' cannot be iterated - its cursor's type is chosen at the moment of "
                "erasure and the vtable does not carry it. Erase the '{}' itself instead.",
                subject.get_type_desciption(), iterator_name));
        }

        if (subject_ct->template_or_self() != iterator_tmpl->template_or_self()) {
            return refuse(fmt::format(
                "'{}' cannot be iterated - it is not an '{}'.",
                subject.get_type_desciption(), iterator_name));
        }

        lookup.plan.kind = AST::IterationSource::t_erased_iterator;
        cursor = subject_ct;
    }
    // (b) the source is itself a cursor - an adaptor, or a cursor a caller already took
    else if (const auto own = AST::conformance_matching_template(subject_ct, iterator_tmpl)) {
        lookup.plan.kind = AST::IterationSource::t_iterator;
        cursor = own->get_complex_type();

        if (cursor == nullptr) {
            return pending();
        }
    }

    if (cursor != nullptr) {
        if (cursor->instantiation_args.size() != 1) {
            return pending();
        }

        lookup.plan.iterator_type = subject;
        lookup.plan.element_type = cursor->instantiation_args[0];
        lookup.plan.key_type = key_type_of(subject, core);
        return lookup;
    }

    // (a) the source is iterable. no substitution machinery here on purpose:
    // TypeRegistry::derive_instantiation already substituted an instantiation's conformances, so
    // `array<int32>` carries `contract::iterable<int32>` and V is read straight off it
    const auto conformances = AST::conformances_matching_template(subject_ct, iterable_tmpl);

    if (conformances.empty()) {
        return refuse(fmt::format(
            "'{}' cannot be iterated - it declares neither '{}' nor '{}'. Declare one, e.g. "
            "'struct {} : {}<...>'.",
            subject.get_type_desciption(), iterator_name, iterable_name,
            subject_ct->template_or_self()->name.value_or("TheType"), iterable_name));
    }

    // two applications of one interface is two element contracts, both legal to declare and neither of
    // them "the" one. a loop cannot pick, so it says so
    if (conformances.size() != 1) {
        return refuse(fmt::format(
            "'{}' declares more than one element contract, so 'foreach' cannot tell which to use. "
            "Take the cursor you mean explicitly - 'foreach ($x->iterate() as ...)'.",
            subject.get_type_desciption()));
    }

    const AST::ComplexType *applied = conformances.front().get_complex_type();

    if (applied == nullptr || applied->instantiation_args.size() != 1) {
        return pending();
    }

    lookup.plan.kind = AST::IterationSource::t_iterable;

    // read straight off the *applied* conformance: TypeRegistry::derive_instantiation already
    // substituted it, so `array<int32>` carries `contract::iterable<int32>` and there is nothing to do here
    lookup.plan.element_type = applied->instantiation_args[0];

    // **the conformance work is done on the template, and the answer substituted** - the redirect and
    // the reason for it both belong to AST::template_conformance_for, which owns it
    const auto redirected = AST::template_conformance_for(subject_ct, conformances.front());

    if (!redirected.has_value()) {
        return pending();
    }

    // **the callee is named through the conformance, not by the string "iterate".** the requirement is
    // whatever stdlib/core/contract.eco declared, and answering_member is what already decides which
    // member satisfies it - asking by name here would be a second answer to that, and could land on a
    // different overload than the one the conformance was checked against
    const std::vector<AST::FunctionDeclNode *> filled =
        AST::interface_implementations(redirected->owner, redirected->conformance, types);

    if (filled.empty() || filled.front() == nullptr) {
        // the conformance is declared but unanswered. AST::TypeChecker reports that at the declaration,
        // where the author can act on it; here it is simply not lowerable yet
        return pending();
    }

    // the *template's* declaration, deliberately. AST::ForeachLowering sets it as the callee with
    // CallSettlement::t_uncoerced, and the monomorphizer's next round binds the owner's parameters from
    // the receiver and rewires it to the instance - the ordinary path every other generic member call
    // takes, rather than a second instantiation route here
    lookup.plan.iterate = filled.front();

    lookup.plan.iterator_type = redirected->to_instance.empty()
        ? filled.front()->get_return_type()
        : AST::substitute_type(filled.front()->get_return_type(), redirected->to_instance, types);

    if (AST::is_undetermined_type(lookup.plan.iterator_type)) {
        return pending();
    }

    lookup.plan.key_type = key_type_of(lookup.plan.iterator_type, core);

    return lookup;
}
