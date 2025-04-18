#include "AST/ASTConformance.h"

#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"

#include <algorithm>
#include <fmt/core.h>

bool AST::conforms_to(const AST::ComplexType *ct, const AST::ValueType &interface)
{
    if (ct == nullptr || !interface.is_interface()) {
        return false;
    }

    const auto &declared = ct->conformances();
    return std::find(declared.begin(), declared.end(), interface) != declared.end();
}

bool AST::conforms_to(const AST::ValueType &type, const AST::ValueType &interface)
{
    if (!type.has_complex_type()) {
        return false;
    }

    return AST::conforms_to(type.get_complex_type(), interface);
}

const std::vector<AST::FunctionDeclNode *> &AST::interface_requirements(const AST::ComplexType *interface)
{
    // the empty answer needs somewhere to live now that this hands back a reference. a function-local
    // static rather than a member of anything, because it is one immutable value shared by every
    // not-an-interface question ever asked
    static const std::vector<AST::FunctionDeclNode *> none;

    if (interface == nullptr || !interface->is_interface_kind()) {
        return none;
    }

    // an interface is never instantiated as a *layout*, but `Iterable<int32>` is still an interned
    // ComplexType with no methods of its own - the requirements are declared on the template. so this
    // is the one place conformance work does take the template_or_self redirect every member lookup
    // takes, and for exactly that reason
    const AST::ComplexType *owner = interface->template_or_self();

    return owner->methods();
}

std::optional<size_t> AST::interface_method_slot(
    const AST::ComplexType *interface, const AST::FunctionDeclNode *requirement)
{
    const std::vector<AST::FunctionDeclNode *> &requirements = AST::interface_requirements(interface);

    for (size_t slot = 0; slot < requirements.size(); slot++) {
        if (requirements[slot] == requirement) {
            return slot;
        }
    }

    return std::nullopt;
}

namespace
{
    // the substitution that turns a requirement's signature into the one an implementor has to answer:
    // the interface's own type parameters bound to the arguments the conformance spelled out.
    // `Iterable<int32>` binds `T` to `int32`, so `next() : ptr<T>` is asked for as `ptr<int32>`
    //
    // empty for a non-generic interface, which is what leaves its requirements compared as written
    AST::TypeSubstitution conformance_substitution(const AST::ValueType &interface)
    {
        const AST::ComplexType *applied = interface.get_complex_type();
        const AST::ComplexType *tmpl = applied->template_or_self();

        if (!applied->is_instantiated() || tmpl->type_parameters.size() != applied->instantiation_args.size()) {
            return AST::TypeSubstitution {};
        }

        return AST::TypeSubstitution::positional(tmpl->type_parameters, applied->instantiation_args);
    }

    // a requirement's parameter or return type as the implementor must spell it. substitute_type needs a
    // registry to re-intern a nested application with, and no reader of this file has one - but it does
    // not need to *create* anything here: every type a requirement mentions was already interned when
    // the interface itself was parsed, and the conformance's own arguments were interned by the clause.
    // so an empty substitution short-circuits and a bound one only ever hits the cache
    AST::ValueType wanted_type(
        const AST::ValueType &declared, const AST::TypeSubstitution &subst, AST::TypeRegistry &registry)
    {
        if (subst.empty()) {
            return declared;
        }

        return AST::substitute_type(declared, subst, registry);
    }

    // a requirement's signature as the implementor has to spell it: the interface's own type parameters
    // resolved through the conformance being checked.
    //
    // built **once per requirement**, not once per candidate - the substitution depends on the
    // requirement and the conformance, never on who is being compared against it, so with k same-named
    // overloads on the implementor the old shape re-substituted every type k times. it is also what the
    // diagnostic renders, so the shape a message names cannot differ from the shape that was compared
    //
    // parameters are held **from index 1**: argument 0 is the receiver, the interface's borrow on the
    // requirement and the implementor's own borrow on the candidate, so comparing it would make every
    // conformance fail
    struct WantedSignature
    {
        const AST::FunctionDeclNode *requirement = nullptr;
        std::vector<AST::ValueType> parameters;
        AST::ValueType return_type;

        // the arity as declared, receiver included - kept rather than derived from `parameters` so a
        // requirement with no arguments at all cannot read as one with a receiver
        size_t arg_count = 0;

        // the requirement rendered as the implementor has to write it. signature_description() shows
        // what was *declared*, which for a generic interface still names its own `T` - a parameter the
        // author of the implementor never wrote and cannot act on
        std::string description() const
        {
            std::string buffer = requirement->func_name() + "(";

            for (size_t i = 0; i < parameters.size(); i++) {
                buffer += (i > 0 ? ", " : "");
                buffer += parameters[i].get_type_desciption();
            }

            return buffer + ") : " + return_type.get_type_desciption();
        }
    };

    WantedSignature wanted_signature(
        const AST::FunctionDeclNode *requirement,
        const AST::TypeSubstitution &subst,
        AST::TypeRegistry &registry)
    {
        WantedSignature wanted;
        wanted.requirement = requirement;
        wanted.arg_count = requirement->args.size();
        wanted.return_type = wanted_type(requirement->get_return_type(), subst, registry);

        wanted.parameters.reserve(requirement->args.size() > 0 ? requirement->args.size() - 1 : 0);

        for (size_t i = 1; i < requirement->args.size(); i++) {
            wanted.parameters.push_back(wanted_type(requirement->parameter_type(i), subst, registry));
        }

        return wanted;
    }

    // does `candidate` answer the requirement?
    //
    // the comparison is ValueType::operator==, which is exact. deliberately not argument_fit's looser
    // ranking: a requirement is a contract, and a method that merely *accepts* what the requirement
    // promises is not the same method a dispatch through a vtable would land on
    bool candidate_answers(const AST::FunctionDeclNode *candidate, const WantedSignature &wanted)
    {
        if (candidate->args.size() != wanted.arg_count) {
            return false;
        }

        // a generic requirement of its own (`function map<U>(...)`) is not something a conformance check
        // can compare: U is bound at the call, not by the conformance. refused at the declaration would
        // be better, and until then this simply never matches
        if (candidate->own_type_param_count() != wanted.requirement->own_type_param_count()) {
            return false;
        }

        for (size_t i = 0; i < wanted.parameters.size(); i++) {
            if (!(candidate->parameter_type(i + 1) == wanted.parameters[i])) {
                return false;
            }
        }

        return candidate->get_return_type() == wanted.return_type;
    }

    // **the one search** for the member of `ct` that answers a requirement, or null.
    //
    // shared by first_unmet_requirement, which reports on the absence, and interface_implementations,
    // which fills the vtable slot - the two readers the header promises cannot disagree about whether a
    // conformance is met. written twice they already had: only the *comparison* was shared, and a rule
    // added to one search (preferring a non-generic candidate, skipping a requirement) would have
    // reached one of them, which is precisely the "reported met, lowered wrong" split this prevents
    AST::FunctionDeclNode *answering_member(const AST::ComplexType *ct, const WantedSignature &wanted)
    {
        for (AST::FunctionDeclNode *candidate :
             AST::find_member_functions(ct, wanted.requirement->func_name())) {
            if (candidate_answers(candidate, wanted)) {
                return candidate;
            }
        }

        return nullptr;
    }
};

std::optional<AST::UnmetRequirement> AST::first_unmet_requirement(
    const AST::ComplexType *ct,
    const AST::ValueType &interface,
    AST::TypeRegistry &types,
    const AST::FunctionRegistry *functions)
{
    if (ct == nullptr || !interface.is_interface()) {
        return std::nullopt;
    }

    const AST::TypeSubstitution subst = conformance_substitution(interface);

    for (const AST::FunctionDeclNode *requirement : AST::interface_requirements(interface.get_complex_type())) {
        if (requirement == nullptr) {
            continue;
        }

        const WantedSignature wanted = wanted_signature(requirement, subst, types);

        // an operator requirement is not on anybody's method table - an operator lives in the root
        // namespace under a decorated name, so the overload set is what has to be asked. a null registry
        // declines the whole question rather than reporting a requirement it cannot look up
        if (requirement->is_operator()) {
            if (functions == nullptr || requirement->ast_namespace == nullptr) {
                continue;
            }

            bool answered = false;
            for (const AST::FunctionDeclNode *candidate :
                 functions->overloads(requirement->func_name(), *requirement->ast_namespace)) {
                if (candidate != requirement && candidate_answers(candidate, wanted)) {
                    answered = true;
                    break;
                }
            }

            if (!answered) {
                return AST::UnmetRequirement { requirement, wanted.description(), "" };
            }

            continue;
        }

        if (answering_member(ct, wanted) != nullptr) {
            continue;
        }

        // the name exists but nothing of that shape does - worth saying, because a wrong return type
        // reads to the author as a missing method otherwise
        const std::vector<AST::FunctionDeclNode *> candidates =
            AST::find_member_functions(ct, requirement->func_name());
        const std::string found = candidates.empty() ? "" : candidates.front()->signature_description();

        return AST::UnmetRequirement { requirement, wanted.description(), found };
    }

    return std::nullopt;
}

std::string AST::interface_erasure_refusal(const AST::ValueType &from, const AST::ValueType &interface)
{
    if (!interface.is_interface()) {
        return "";
    }

    // **the asymmetry the whole design rests on.** a struct's conformance is a compile-time contract: it
    // has no block, no typeinfo word and nothing to dispatch through, so there is no erased value to build.
    // the constraint path is what a struct uses, and it costs nothing at runtime
    if (from.is_struct()) {
        return fmt::format(
            "'{}' is a struct, so it cannot be stored as '{}' - a struct carries no runtime type to "
            "dispatch through. Take it through a constrained generic instead, e.g. "
            "'function f<T: {}>(T& $v)'.",
            from.get_type_desciption(),
            interface.get_type_desciption(),
            interface.get_complex_type()->name.value_or("TheInterface"));
    }

    if (!from.is_class()) {
        return "";
    }

    // **a generic implementor has no method bodies to point a vtable at.** a method of a template is
    // instantiated per *call site* by the monomorphizer, and a vtable slot is not a call site - so the
    // table would reference a symbol nothing ever emits. the constraint path works for a generic today,
    // which is what makes this a hole rather than a wall
    if (from.get_complex_type() != nullptr && from.get_complex_type()->is_instantiated()) {
        return fmt::format(
            "'{}' is a generic instantiation, and storing one as '{}' is not supported yet - a vtable "
            "needs a body per requirement, which a template only gets per call site. Take it through a "
            "constrained generic instead.",
            from.get_type_desciption(), interface.get_type_desciption());
    }

    // **an operator requirement has no vtable slot.** an operator is a free declaration in the root
    // namespace with no receiver, so there is nothing for a dispatch to key on. such an interface is still
    // perfectly usable as a constraint and on the right of `instanceof` - it just is not a type
    for (const AST::FunctionDeclNode *requirement :
         AST::interface_requirements(interface.get_complex_type())) {
        if (requirement != nullptr && requirement->is_operator()) {
            return fmt::format(
                "'{}' requires '{}', and an operator has no receiver to dispatch through - so '{}' cannot "
                "be stored as a value. Use it as a constraint or with 'instanceof' instead.",
                interface.get_type_desciption(),
                requirement->func_name(),
                interface.get_type_desciption());
        }
    }

    return "";
}

std::vector<AST::FunctionDeclNode *> AST::interface_implementations(
    const AST::ComplexType *ct, const AST::ValueType &interface, AST::TypeRegistry &types)
{
    if (ct == nullptr || !interface.is_interface()) {
        return {};
    }

    const AST::TypeSubstitution subst = conformance_substitution(interface);
    const std::vector<AST::FunctionDeclNode *> &requirements =
        AST::interface_requirements(interface.get_complex_type());

    std::vector<AST::FunctionDeclNode *> filled;
    filled.reserve(requirements.size());

    for (AST::FunctionDeclNode *requirement : requirements) {
        if (requirement == nullptr) {
            return {};
        }

        // an operator has no receiver, so there is no slot to dispatch it through. it still has to be
        // *satisfied* - first_unmet_requirement checks it against the root overload set - but a vtable
        // cannot hold it, which is why an interface declaring one is not a storable type
        if (requirement->is_operator()) {
            filled.push_back(nullptr);
            continue;
        }

        AST::FunctionDeclNode *found = answering_member(ct, wanted_signature(requirement, subst, types));

        // unanswered, so there is no table to hand back. the diagnostic is first_unmet_requirement's,
        // reported once at the declaration rather than again at every widening
        if (found == nullptr) {
            return {};
        }

        filled.push_back(found);
    }

    return filled;
}
