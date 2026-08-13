#include "AST/ASTUnwrap.h"

#include "AST/ASTConformance.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTNullability.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

namespace
{
    AST::UnwrapLookup refuse(std::string reason)
    {
        AST::UnwrapLookup lookup;
        lookup.result = AST::UnwrapLookup::Result::t_refused;
        lookup.refusal = std::move(reason);
        return lookup;
    }

    AST::UnwrapLookup pending()
    {
        AST::UnwrapLookup lookup;
        lookup.result = AST::UnwrapLookup::Result::t_pending;
        return lookup;
    }

    // `contract::failable<E>` on the subject, if it declares one. **absent is not an error here** - only
    // an `else ($e)` asking for one makes it one, and that refusal belongs at the `$e`. the same
    // question AST::key_type_of asks of `contract::keyed<K>`, through the one answer to it
    std::optional<AST::ValueType> failure_type_of(
        const AST::ValueType &subject,
        const AST::CoreTypes &core
    )
    {
        return AST::sole_conformance_argument(
            subject, core.declared_template(AST::CoreTypeKind::t_failable));
    }
}

AST::UnwrapLookup AST::unwrap_plan_for(
    const AST::ValueType &subject,
    const AST::CoreTypes &core,
    AST::TypeRegistry &types
)
{
    // **the nullable arm is first, and that is the opposite of AST::iteration_plan_for's order.**
    //
    // `foreach` has no meaning without the iteration protocol, so a missing binding has to be a located
    // refusal there. `guard` had a meaning before the unwrapping protocol existed and keeps it: a `T?` is
    // answered by the compiler, its payload is a property of the *type* and nothing later can change it,
    // so this arm is byte for byte what `guard` always did. two things fall out of it being first - a
    // program that guards a nullable still compiles with `--no-stdlib`, and contract.eco itself stays
    // compilable by a compiler mid-parse of these very interfaces
    if (subject.is_nullable()) {
        AST::UnwrapLookup lookup;
        lookup.result = AST::UnwrapLookup::Result::t_ok;
        lookup.plan.kind = AST::UnwrapSource::t_builtin_nullable;
        lookup.plan.payload_type = AST::unwrapped_type_of(subject);
        return lookup;
    }

    // not settled yet - a type parameter awaiting substitution, or a call that has not resolved. asking
    // again next round is the answer, and it must not read as a refusal
    if (AST::is_undetermined_type(subject)) {
        return pending();
    }

    const AST::ComplexType *unwrappable_tmpl =
        core.declared_template(AST::CoreTypeKind::t_unwrappable);

    if (unwrappable_tmpl == nullptr) {
        return refuse(fmt::format(
            "'{}' cannot be guarded: it is not nullable, and nothing in this program declares "
            "'#[core: unwrappable]' for it to conform to. that interface lives in the standard library, "
            "which this compilation left out - a 'T?' still guards without it.",
            subject.get_type_desciption()));
    }

    // the interface as the stdlib spells it. every refusal below quotes this rather than a literal, so
    // moving or renaming it moves what the diagnostics tell the author to write
    const std::string unwrappable_name = core.spelling(AST::CoreTypeKind::t_unwrappable);

    // the subject is read *through* a borrow exactly as every other reader reads one, and the const is
    // kept rather than peeled with it - AST::iteration_plan_for's rule, and here it is what makes the
    // const refusal below reachable at all
    const AST::ValueType peeled = AST::target_type_of(subject);

    if (!peeled.has_complex_type()) {
        // a primitive, a pointer or a callable cannot declare a conformance, so the old sentence is
        // still the whole truth for this arm - and it is the one the other two optional forms share
        return refuse(AST::certainly_present_refusal(AST::OptionalForm::t_guard, subject));
    }

    if (peeled.is_const()) {
        return refuse(fmt::format(
            "'{}' cannot be guarded through a 'const' - taking the value out hands back a borrow that "
            "may be written, and '{}::unwrap()' is not declared const. guard a value nobody promised to "
            "leave alone.",
            subject.get_type_desciption(), unwrappable_name));
    }

    AST::ComplexType *subject_ct = peeled.get_complex_type();

    AST::UnwrapLookup lookup;
    lookup.result = AST::UnwrapLookup::Result::t_ok;
    lookup.plan.kind = AST::UnwrapSource::t_protocol;

    // **an erased value is the same kind, not a second one.** find_member_functions finds a requirement
    // in the same `_methods` list an implementation would be in, and gen_function_call already routes on
    // FunctionDeclNode::is_interface_requirement() - so the lowering needs no arm for this at all.
    //
    // it did not run at first, and not for a reason that lived here: a call through an erased *generic*
    // interface had no vtable slot, AST::interface_method_slot matching the requirement by pointer
    // identity against the template's methods while the call carries the instantiation's. that was
    // `todo/B53`, it predated this protocol - `foreach` over an erased `contract::iterator<V>` failed the
    // same way, which is why `IterationSource::t_erased_iterator` had never been exercised either - and
    // it is fixed. `tests_eco/nullability/unwrappable_erased` is this arm
    if (peeled.is_interface()) {
        if (subject_ct->template_or_self() != unwrappable_tmpl->template_or_self()) {
            return refuse(fmt::format(
                "'{}' cannot be guarded - it is not a '{}'.",
                subject.get_type_desciption(), unwrappable_name));
        }

        if (subject_ct->instantiation_args.size() != 1) {
            return pending();
        }

        lookup.plan.payload_type = subject_ct->instantiation_args[0];
        lookup.plan.failure_type = failure_type_of(peeled, core);
        return lookup;
    }

    const std::vector<AST::ValueType> conformances =
        AST::conformances_matching_template(subject_ct, unwrappable_tmpl);

    // **a purpose-built sentence rather than a composed one.** this arm is a genuinely different
    // situation from the primitive above: the type *could* conform and does not, so there are two
    // remedies and the message owes the author both. AST::certainly_present_refusal already ends on
    // "or drop the guard", and appending a second "or" to it read as a sentence nobody wrote
    if (conformances.empty()) {
        return refuse(fmt::format(
            "'{}' cannot be guarded: it is not nullable, and it declares no '{}'. write '{}?' if the "
            "value may be absent, or declare 'struct {} : {}<V>' to make the type itself unwrappable.",
            subject.get_type_desciption(),
            unwrappable_name,
            subject.get_type_desciption(),
            subject_ct->template_or_self()->name.value_or("TheType"),
            unwrappable_name));
    }

    // more than one application and there is no answer to "which value does this bind", so the program
    // has to say. AST::iteration_plan_for makes the same call for the same reason
    if (conformances.size() != 1) {
        return refuse(fmt::format(
            "'{}' declares more than one '{}', so 'guard' cannot tell which value to bind. unwrap the "
            "one you mean explicitly.",
            subject.get_type_desciption(), unwrappable_name));
    }

    const AST::ComplexType *applied = conformances.front().get_complex_type();

    if (applied == nullptr || applied->instantiation_args.size() != 1) {
        return pending();
    }

    // V is read straight off the applied conformance - TypeRegistry::derive_instantiation already
    // substituted it, so there is no substitution machinery here
    lookup.plan.payload_type = applied->instantiation_args[0];

    // **the requirements have to be answered, not merely declared.** a conformance whose `unwrap()` is
    // missing is AST::TypeChecker's diagnostic to report; here it is simply not lowerable yet, and
    // saying so as `t_pending` is what keeps the two from doubling up on one mistake
    const auto redirected = AST::template_conformance_for(subject_ct, conformances.front());

    if (!redirected.has_value()) {
        return pending();
    }

    const std::vector<AST::FunctionDeclNode *> filled =
        AST::interface_implementations(redirected->owner, redirected->conformance, types);

    if (filled.empty()) {
        return pending();
    }

    for (auto *implementation : filled) {
        if (implementation == nullptr) {
            return pending();
        }
    }

    lookup.plan.failure_type = failure_type_of(peeled, core);

    return lookup;
}

std::string AST::guard_payload_refusal(
    const AST::ValueType &written,
    const AST::ValueType &payload,
    const AST::ValueType &subject
)
{
    // an undetermined payload is not a mismatch, it is a question nobody has answered yet - and judging
    // it here would be a round too early for exactly AST::unwrap_plan_for's `t_pending` reason
    if (AST::is_undetermined_type(payload)) {
        return {};
    }

    // is_implicitly_convertible rather than equality, so a written type may widen the payload the way an
    // ordinary declaration's would
    if (AST::is_implicitly_convertible(payload, written)) {
        return {};
    }

    return fmt::format(
        "'{}' cannot hold the '{}' inside a '{}'",
        written.get_type_desciption(),
        payload.get_type_desciption(),
        subject.get_type_desciption());
}
