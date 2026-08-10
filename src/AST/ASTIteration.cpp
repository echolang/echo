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
    const AST::ValueType &source,
    const AST::CoreTypes &core,
    AST::TypeRegistry &types
)
{
    // **the unbound case comes first.** `--no-stdlib` is a legitimate program, and this is also what
    // keeps stdlib/core/contract.eco itself compilable - it is parsed by the very compiler that would
    // otherwise assert on the interfaces not existing yet
    const AST::ComplexType *iterator_tmpl = core.declared_template(AST::CoreTypeKind::t_iterator);
    const AST::ComplexType *iterable_tmpl = core.declared_template(AST::CoreTypeKind::t_iterable);

    // the read-only half may legitimately be absent while the other two are bound - it is younger than
    // they are, and a program is free to declare its own protocol. so it is not part of the gate below:
    // a null one simply means no value in this program iterates through a const, which the refusal says.
    // every lookup it is handed to already answers "no match" for a null template, so it needs no guard
    const AST::ComplexType *const_iterable_tmpl = core.declared_template(AST::CoreTypeKind::t_const_iterable);

    if (iterator_tmpl == nullptr || iterable_tmpl == nullptr) {
        return refuse(
            "'foreach' needs the iteration protocol, and nothing in this program declares "
            "'#[core: iterator]'. it lives in the standard library, which this compilation left out.");
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
    const std::string const_iterable_name = core.spelling(AST::CoreTypeKind::t_const_iterable);

    // the loop reads *through* a borrow exactly as every other reader does; `foreach ($a as ...)` over a
    // `array<int32>&` parameter is the ordinary case, not a special one
    //
    // **the borrow is peeled and the const is not.** they used to go together, and the result was that
    // `const array<int32>` and `array<int32>` reached this function as one question - so no answer here
    // could depend on which of them the loop was handed, and the const path was unreachable by
    // construction. nothing below needs the const stripped: every lookup here reads the ComplexType, and
    // a flag on the level above it never changed which one that is
    const AST::ValueType subject = AST::target_type_of(source);
    const bool subject_is_const = subject.is_const();

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
        // **both halves of the protocol, not just the writable one.** an erased `const_iterable` has the
        // identical hole for the identical reason, and naming one template here is what would let it fall
        // through to "it is not an iterator" - a sentence about the wrong thing
        const AST::ComplexType *erased_tmpl = subject_ct->template_or_self();

        if (erased_tmpl == iterable_tmpl->template_or_self()
            || (const_iterable_tmpl != nullptr
                && erased_tmpl == const_iterable_tmpl->template_or_self())) {
            return refuse(fmt::format(
                "an erased '{}' cannot be iterated - its cursor's type is chosen at the moment of "
                "erasure and the vtable does not carry it. Erase the '{}' itself instead.",
                subject.get_type_desciption(), iterator_name));
        }

        if (erased_tmpl != iterator_tmpl->template_or_self()) {
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

        // **a cursor is spent by being read**, which is the one place const cannot be worked around by
        // handing back something weaker: `advance()` moves the cursor itself, so there is no read-only
        // form of it to select. refused here rather than left to the `advance()` call, which would report
        // against a receiver AST::ForeachLowering synthesized and a token the author never wrote
        if (subject_is_const) {
            return refuse(fmt::format(
                "'{}' cannot be iterated - stepping a cursor writes to the cursor, and '{}::advance()' "
                "is not declared const. Iterate what it was taken from instead.",
                subject.get_type_desciption(), iterator_name));
        }

        lookup.plan.iterator_type = subject;
        lookup.plan.element_type = cursor->instantiation_args[0];
        lookup.plan.key_type = key_type_of(subject, core);
        return lookup;
    }

    // (a) the source is iterable. no substitution machinery here on purpose:
    // TypeRegistry::derive_instantiation already substituted an instantiation's conformances, so
    // `array<int32>` carries `contract::iterable<int32>` and V is read straight off it
    //
    // **which of the two contracts answers is decided by the value the loop was handed**, and this is
    // the whole of the const path: a const receiver may only be given a cursor over storage it may not
    // write, that promise belongs to the *requirement*, and a requirement's receiver is compared exactly
    // - so the two `iterate()`s answer two interfaces rather than forming an overload set. everything
    // below this point is unchanged by which one it was
    //
    // a mutable subject prefers the writable contract, because a type declaring both means that one by
    // it. the fallback is what lets a type declaring only the read-only half - one whose elements are
    // const however it was reached - still loop from a mutable place
    const std::vector<AST::ValueType> writable =
        AST::conformances_matching_template(subject_ct, iterable_tmpl);
    const std::vector<AST::ValueType> readable =
        AST::conformances_matching_template(subject_ct, const_iterable_tmpl);

    const std::vector<AST::ValueType> &conformances =
        !subject_is_const && !writable.empty() ? writable : readable;

    if (conformances.empty()) {
        // **the type is iterable and only the read-only half is missing.** worth its own sentence: the
        // generic one below says "declares neither", which is false here and sends the author to add a
        // conformance they can see in front of them
        if (subject_is_const && !writable.empty()) {
            return refuse(fmt::format(
                "'{}' cannot be iterated through a 'const' - it declares '{}' but not '{}', so the only "
                "cursor it can hand back is one that may write. Declare '{}' beside it over the const "
                "element, or iterate a value nobody promised to leave alone.",
                subject.get_type_desciption(), iterable_name, const_iterable_name, const_iterable_name));
        }

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
