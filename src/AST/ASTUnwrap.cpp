#include "AST/ASTUnwrap.h"

#include "AST/ASTConformance.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTModule.h"
#include "AST/ASTNullability.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"

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

    // pending, and **AST::TypeChecker owns the sentence** - see UnwrapLookup::reported_elsewhere. every
    // caller here is a conformance that is declared and not answered, which is an UnmetInterfaceRequirement
    // at the implementor and nothing this function should speak about
    AST::UnwrapLookup unanswered()
    {
        AST::UnwrapLookup lookup = pending();
        lookup.reported_elsewhere = true;
        return lookup;
    }

    // **which requirement of `contract::unwrappable<V>` is which.** the order
    // AST::interface_requirements answers in, which that function documents as declaration order *and
    // therefore vtable slot order* - the same positional fact AST::interface_method_slot rests on and
    // `tests_eco/nullability/unwrappable_erased` pins by name in the emitted table.
    //
    // **this file is the one place in the compiler that knows which slot is which**, which is the whole
    // point: if the requirements' *names* were string literals in AST::GuardLowering,
    // stdlib/core/contract.eco could not rename what it declares without silently breaking a pass
    enum : size_t
    {
        t_slot_has_value = 0,
        t_slot_unwrap = 1,
        t_unwrappable_slot_count = 2,
    };

    // **is this list the two slots above, in that order?** the arity and the two shapes together.
    //
    // the shape half never *chooses* - the positions above are the answer - so it is not a second answer
    // to "which requirement is this". what it is, is what turns "somebody reordered contract.eco" from
    // two silently swapped calls into a pending answer the finalizing round speaks up about.
    // `has_value()` answers bool, `unwrap()` hands back a borrow
    bool slots_are_the_protocol(const std::vector<AST::FunctionDeclNode *> &slots)
    {
        if (slots.size() != t_unwrappable_slot_count
            || slots[t_slot_has_value] == nullptr || slots[t_slot_unwrap] == nullptr) {
            return false;
        }

        return slots[t_slot_has_value]->get_return_type().is_boolean_type()
            && slots[t_slot_unwrap]->get_return_type().is_pointer();
    }

    // **binds `contract::failable<E>` onto the plan, or hands back the answer its caller owes.**
    // the type and the `failure()` that answers it are one question and not two, so they are bound
    // together or not at all.
    //
    // **absent is not an error here**: only an `else ($e)` asking for one makes it one, and that refusal
    // belongs at the `$e`. the same shape AST::key_type_of asks of `contract::keyed<K>`, through the one
    // answer to it.
    //
    // it returns the caller's own answer rather than a record the caller then has to translate, because
    // the two shapes of "no answer yet" are not the same answer and both arms below spell the
    // translation: `pending()` is a half-derived instantiation, which a later round leaves behind,
    // and `unanswered()` is a conformance declared with its `failure()` missing, which no round will ever
    // fix and which AST::TypeChecker reports at the implementor. only the second silences the asker - see
    // UnwrapLookup::reported_elsewhere. **nullopt is the success**: the plan has been written, carry on
    std::optional<AST::UnwrapLookup> bind_failure(
        AST::UnwrapPlan &plan,
        AST::ComplexType *subject_ct,
        const AST::ValueType &subject,
        const AST::CoreTypes &core,
        AST::TypeRegistry &types
    )
    {
        const AST::ComplexType *failable_tmpl = core.declared_template(AST::CoreTypeKind::t_failable);

        const std::optional<AST::ValueType> failure_type =
            AST::sole_conformance_argument(subject, failable_tmpl);

        // **absent is the ordinary case and not any kind of "yet"**: a subject that declares no
        // `failable` simply has no reason to bind, which only an `else ($e)` makes a mistake
        if (!failure_type.has_value()) {
            return std::nullopt;
        }

        // the conformance the type argument was just read off, taken back to the template the
        // requirements live on - AST::template_conformance_for's redirect, exactly as the unwrappable
        // lookup below takes it
        const auto applied = AST::conformance_matching_template(subject_ct, failable_tmpl);

        if (!applied.has_value()) {
            return pending();
        }

        const auto redirected = AST::template_conformance_for(subject_ct, applied.value());

        if (!redirected.has_value()) {
            return pending();
        }

        // one requirement, one slot: `contract::failable<E>` declares only `failure()`
        const std::vector<AST::FunctionDeclNode *> filled =
            AST::interface_implementations(redirected->owner, redirected->conformance, types);

        if (filled.size() != 1 || filled.front() == nullptr) {
            return unanswered();
        }

        plan.failure_type = failure_type;
        plan.failure = filled.front();

        return std::nullopt;
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
    // it predated this protocol: `foreach` over an erased `contract::iterator<V>` failed the
    // same way, which is why `IterationSource::t_erased_iterator` had never been exercised either.
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

        // **the requirements themselves are the callees here**, which is why this arm costs the lowering
        // nothing: AST::find_member_functions would have found them in this same `_methods` list, and
        // the dispatch through the vtable is ExprCodegen::gen_function_call's question, asked of
        // FunctionDeclNode::is_interface_requirement(). read through interface_requirements so the
        // `template_or_self` redirect is taken once and in one place - a generic interface's own
        // application declares nothing, which is what the template_or_self redirect settled
        const std::vector<FunctionDeclNode *> &requirements = AST::interface_requirements(subject_ct);

        if (!slots_are_the_protocol(requirements)) {
            return pending();
        }

        lookup.plan.has_value = requirements[t_slot_has_value];
        lookup.plan.unwrap = requirements[t_slot_unwrap];

        // an erased value's own ComplexType is the interface application, and an interface declares no
        // conformances - so a `failable` never applies here, and asking is what says so once
        if (auto answer = bind_failure(lookup.plan, subject_ct, peeled, core, types)) {
            return *answer;
        }

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
    // missing is AST::TypeChecker's diagnostic to report; here it is simply not lowerable, ever, and
    // saying so as `unanswered()` is what keeps the two from doubling up on one mistake
    const auto redirected = AST::template_conformance_for(subject_ct, conformances.front());

    if (!redirected.has_value()) {
        return pending();
    }

    // **and the declarations it answered with are kept, not counted and thrown away.** this lookup used
    // to run purely to test the set was non-empty, and AST::GuardLowering then minted `has_value` and
    // `unwrap` by name and let the matcher choose all over again - a second answer to the question this
    // very call had just answered. AST::iteration_plan_for states the rule in so many words
    const std::vector<AST::FunctionDeclNode *> filled =
        AST::interface_implementations(redirected->owner, redirected->conformance, types);

    // **empty is the unanswered conformance and is not the same as a wrong shape.** an empty list is
    // precisely when first_unmet_requirement returns a value, so the implementor already has a diagnostic
    // coming; a list of the right length carrying the wrong shapes is stdlib/core/contract.eco having
    // been reordered, which nobody else reports and which therefore has to stay loud
    if (filled.empty()) {
        return unanswered();
    }

    if (!slots_are_the_protocol(filled)) {
        return pending();
    }

    // the *template's* declarations, deliberately - AST::interface_implementations reads them off
    // `redirected->owner`. AST::GuardLowering sets them as callees with CallSettlement::t_uncoerced and
    // the monomorphizer's next round binds the owner's parameters from the receiver and rewires the call
    // to the instance: the ordinary path every other generic member call takes, and
    // AST::ForeachLowering's `plan.iterate` for the same reason
    lookup.plan.has_value = filled[t_slot_has_value];
    lookup.plan.unwrap = filled[t_slot_unwrap];

    if (auto answer = bind_failure(lookup.plan, subject_ct, peeled, core, types)) {
        return *answer;
    }

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

AST::FunctionDeclNode &AST::ensure_unwrap_abort(AST::Module &module, const TokenReference &at)
{
    if (module.unwrap_abort != nullptr) {
        return *module.unwrap_abort;
    }

    auto &decl = module.nodes.emplace_back<FunctionDeclNode>(
        module.make_virtual_token("unwrap_abort", Token::Type::t_identifier, at));

    decl.builtin = "unwrap_abort";
    decl.is_implicitly_generated = true;
    decl.return_type = &module.nodes.emplace_back<TypeNode>(ValueType::void_type());

    module.unwrap_abort = &decl;
    return decl;
}
