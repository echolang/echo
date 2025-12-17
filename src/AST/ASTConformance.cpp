#include "AST/ASTConformance.h"

#include "AST/ASTConstness.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"
#include "AST/ASTTypeUnify.h"
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

    // an interface is never instantiated as a *layout*, but `contract::iterable<int32>` is still an interned
    // ComplexType with no methods of its own - the requirements are declared on the template. so this
    // is the one place conformance work does take the template_or_self redirect every member lookup
    // takes, and for exactly that reason
    const AST::ComplexType *owner = interface->template_or_self();

    return owner->methods();
}

const std::vector<AST::TypeParamDecl *> &AST::interface_associated_types(const AST::ComplexType *interface)
{
    static const std::vector<AST::TypeParamDecl *> none;

    if (interface == nullptr || !interface->is_interface_kind()) {
        return none;
    }

    // the same redirect interface_requirements takes, and for the same reason: `contract::iterable<int32>` is
    // an interned ComplexType that declares nothing of its own
    return interface->template_or_self()->associated_types();
}

std::vector<AST::ValueType> AST::conformances_matching_template(
    const AST::ComplexType *ct,
    const AST::ComplexType *interface_template
)
{
    std::vector<AST::ValueType> found;

    if (ct == nullptr || interface_template == nullptr) {
        return found;
    }

    for (const AST::ValueType &declared : ct->conformances()) {
        const AST::ComplexType *applied = declared.get_complex_type();

        if (applied != nullptr && applied->template_or_self() == interface_template->template_or_self()) {
            found.push_back(declared);
        }
    }

    return found;
}

std::optional<AST::ValueType> AST::conformance_matching_template(
    const AST::ComplexType *ct,
    const AST::ComplexType *interface_template
)
{
    if (ct == nullptr || interface_template == nullptr) {
        return std::nullopt;
    }

    // its own scan rather than the vector form's `.front()`: this is asked per `foreach` per fixpoint
    // round, and "is there exactly one" needs no list built to answer - the second match ends it
    std::optional<AST::ValueType> found;

    for (const AST::ValueType &declared : ct->conformances()) {
        const AST::ComplexType *applied = declared.get_complex_type();

        if (applied == nullptr
            || applied->template_or_self() != interface_template->template_or_self()) {
            continue;
        }

        // two answers is not one answer. a caller that wants to word "which of these did you mean" asks
        // the vector form; this one only says it cannot tell
        if (found.has_value()) {
            return std::nullopt;
        }

        found = declared;
    }

    return found;
}

std::optional<AST::TemplateConformance> AST::template_conformance_for(
    AST::ComplexType *ct,
    const AST::ValueType &applied
)
{
    if (ct == nullptr) {
        return std::nullopt;
    }

    AST::TemplateConformance result;
    result.owner = ct;
    result.conformance = applied;

    if (!ct->is_instantiated()) {
        return result;
    }

    AST::ComplexType *tmpl = ct->template_ref;

    if (tmpl == nullptr || tmpl->type_parameters.size() != ct->instantiation_args.size()) {
        return std::nullopt;
    }

    const std::vector<AST::ValueType> &declared = ct->conformances();
    const auto found = std::find(declared.begin(), declared.end(), applied);

    if (found == declared.end()) {
        return std::nullopt;
    }

    // index-parallel by construction - see the header
    const size_t slot = static_cast<size_t>(found - declared.begin());

    if (slot >= tmpl->conformances().size()) {
        return std::nullopt;
    }

    result.owner = tmpl;
    result.conformance = tmpl->conformances()[slot];
    result.to_instance =
        AST::TypeSubstitution::positional(tmpl->type_parameters, ct->instantiation_args);

    return result;
}

std::optional<size_t> AST::interface_method_slot(
    const AST::ComplexType *interface,
    const AST::FunctionDeclNode *requirement
)
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
    // the interface's own type parameters bound to the arguments the conformance spelled out.
    // `contract::iterable<int32>` binds `T` to `int32`, so `next() : ptr<T>` is asked for as `ptr<int32>`
    //
    // empty for a non-generic interface, which is what leaves its requirements compared as written.
    // this is only *half* of what a conformance binds - AST::conformance_bindings adds the associated
    // types on top of it, and is what every caller outside this helper asks
    AST::TypeSubstitution interface_parameter_substitution(const AST::ValueType &interface)
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

        // the one thing about argument 0 that *is* compared. the receiver's type differs by
        // construction - the interface borrows itself, the implementor borrows itself - but its
        // const-ness is the requirement's promise to the caller, not an artefact of who declared it:
        // a `const` requirement says a holder of the interface may call this on a const value, and a
        // vtable slot filled by a method that may write would launder exactly that promise away
        bool receiver_is_const = false;

        // the requirement rendered as the implementor has to write it. signature_description() shows
        // what was *declared*, which for a generic interface still names its own `T` - a parameter the
        // author of the implementor never wrote and cannot act on
        std::string description() const
        {
            std::string buffer = std::string(receiver_is_const ? "const " : "") + requirement->func_name() + "(";

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
        wanted.receiver_is_const = AST::receiver_is_const(*requirement);
        wanted.return_type = wanted_type(requirement->get_return_type(), subst, registry);

        wanted.parameters.reserve(requirement->args.size() > 0 ? requirement->args.size() - 1 : 0);

        for (size_t i = 1; i < requirement->args.size(); i++) {
            wanted.parameters.push_back(wanted_type(requirement->parameter_type(i), subst, registry));
        }

        return wanted;
    }

    // **the half of "does this candidate answer" that is settled before a single type is compared** -
    // arity, the candidate's own generic parameters, and the receiver's const-ness.
    //
    // one predicate because it has two readers that cannot compare types the same way. candidate_answers
    // below compares them exactly; conformance_bindings' trial loop *unifies* them instead, because it is
    // the thing discovering what the associated types are and has nothing to compare against yet. gated
    // on arity alone, that loop bound `Iter` off whichever half of an `iterate()` overload set came first
    // - the mutable one - and `contract::const_iterable` was then reported unmet against a cursor it had
    // never asked for, at the declaration, naming a type the author had written correctly
    bool candidate_shape_answers(const AST::FunctionDeclNode *candidate, const WantedSignature &wanted)
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

        // exact, and in both directions: a `const` requirement is not answered by a method that may
        // write, and a plain one is not answered by a const method either - the vtable holds one
        // declaration, and a caller through the interface has to be able to read which promise it got
        // off the requirement alone. it is also what makes a receiver-split overload set answer *two*
        // interfaces, which is the whole of how a container offers a writable cursor and a read-only one
        return AST::receiver_is_const(*candidate) == wanted.receiver_is_const;
    }

    // does `candidate` answer the requirement?
    //
    // the comparison is ValueType::operator==, which is exact. deliberately not argument_fit's looser
    // ranking: a requirement is a contract, and a method that merely *accepts* what the requirement
    // promises is not the same method a dispatch through a vtable would land on
    bool candidate_answers(const AST::FunctionDeclNode *candidate, const WantedSignature &wanted)
    {
        if (!candidate_shape_answers(candidate, wanted)) {
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

    // does this signature still mention an associated type nothing has bound yet?
    bool mentions_unbound(const WantedSignature &wanted, const AST::TypeSubstitution &subst,
        const std::vector<AST::TypeParamDecl *> &associated)
    {
        for (const AST::TypeParamDecl *assoc : associated) {
            if (subst.covers(assoc)) {
                continue;
            }

            if (AST::contains_type_param(wanted.return_type, assoc)) {
                return true;
            }

            for (const AST::ValueType &param : wanted.parameters) {
                if (AST::contains_type_param(param, assoc)) {
                    return true;
                }
            }
        }

        return false;
    }
};

AST::ConformanceBinding AST::conformance_bindings(
    const AST::ComplexType *ct,
    const AST::ValueType &interface,
    AST::TypeRegistry &types
)
{
    AST::ConformanceBinding result;

    if (!interface.is_interface()) {
        return result;
    }

    result.substitution = interface_parameter_substitution(interface);

    const std::vector<AST::TypeParamDecl *> &associated =
        AST::interface_associated_types(interface.get_complex_type());

    // the overwhelmingly common case, and the one every interface written before this feature is in:
    // nothing to solve, so nothing is walked
    if (ct == nullptr || associated.empty()) {
        return result;
    }

    // @TODO the explicit `type Iter = X;` form - see the chapter's holes - would seed the trial here
    // rather than replace it: the requirements below are still re-checked against the seed, so a wrong
    // explicit binding stays an ordinary unmet-requirement diagnostic rather than a second failure mode
    for (const AST::FunctionDeclNode *requirement :
         AST::interface_requirements(interface.get_complex_type())) {
        if (requirement == nullptr || requirement->is_operator()) {
            continue;
        }

        const WantedSignature wanted = wanted_signature(requirement, result.substitution, types);

        // only a requirement that still mentions an unbound associated type can teach us anything; every
        // other one is a pure check, and checking is first_unmet_requirement's job rather than this one's
        if (!mentions_unbound(wanted, result.substitution, associated)) {
            continue;
        }

        for (const AST::FunctionDeclNode *candidate :
             AST::find_member_functions(ct, requirement->func_name())) {
            if (!candidate_shape_answers(candidate, wanted)) {
                continue;
            }

            AST::TypeSubstitution trial;

            // **allow_decay = false is load-bearing.** the default carries the two *call boundary*
            // rules - pointer decay and auto-borrow - and a conformance is not a call boundary: with
            // decay on, `iterate() : Iter` would happily bind `Iter` through a pointer level the
            // implementor does not actually return
            bool unified = AST::unify_type(
                wanted.return_type, candidate->get_return_type(), trial, /*allow_decay=*/false);

            for (size_t i = 0; unified && i < wanted.parameters.size(); i++) {
                unified = AST::unify_type(
                    wanted.parameters[i], candidate->parameter_type(i + 1), trial, /*allow_decay=*/false);
            }

            if (!unified) {
                continue;
            }

            // **keep only the associated bindings.** unify_type binds *any* bare type parameter it
            // meets, the interface's own `V` included, and TypeSubstitution::bind silently replaces on
            // rebind - so a candidate that merely has the right shape could quietly weaken the contract
            // it is supposed to answer. discarding everything else is what leaves ValueType::operator==
            // as the thing that decides, with unification only ever proposing
            for (const AST::TypeParamDecl *assoc : associated) {
                if (!trial.covers(assoc)) {
                    continue;
                }

                const AST::ValueType proposed = *trial.lookup(assoc);

                if (const AST::ValueType *already = result.substitution.lookup(assoc)) {
                    // **two members proposed different types for one name.** solve order decides which
                    // came first; saying so is better than silently taking it
                    if (!(*already == proposed)) {
                        result.failure = AST::ConformanceBinding::Failure::t_ambiguous;
                        result.associated = assoc;
                        result.first = *already;
                        result.second = proposed;
                        return result;
                    }

                    continue;
                }

                // the associated type's own constraint, **substituted through this conformance**:
                // `type Iter : contract::iterator<V>` means nothing until `V` is bound, which is exactly why
                // AST::constraint_admits exists apart from TypeParamDecl::allows
                std::vector<AST::ValueType> constraint;
                constraint.reserve(assoc->constraint.size());
                for (const AST::ValueType &atom : assoc->constraint) {
                    constraint.push_back(wanted_type(atom, result.substitution, types));
                }

                if (!AST::constraint_admits(constraint, proposed)) {
                    result.failure = AST::ConformanceBinding::Failure::t_constraint;
                    result.associated = assoc;
                    result.first = proposed;

                    for (const AST::ValueType &atom : constraint) {
                        if (!result.constraint_spelling.empty()) {
                            result.constraint_spelling += "|";
                        }
                        result.constraint_spelling += atom.get_type_desciption();
                    }

                    return result;
                }

                result.substitution.bind(assoc, proposed);
            }

            // the first candidate that unifies decides. a smarter constraint-propagating solver would
            // accept a few more programs; this one is order-dependent and says so, and the escape hatch
            // is that the requirements are all re-checked exactly afterwards
            break;
        }
    }

    return result;
}

std::optional<AST::UnmetRequirement> AST::first_unmet_requirement(
    const AST::ComplexType *ct,
    const AST::ValueType &interface,
    AST::TypeRegistry &types,
    const AST::FunctionRegistry *functions
)
{
    if (ct == nullptr || !interface.is_interface()) {
        return std::nullopt;
    }

    // the *full* binding - the interface's own parameters and its associated types - so that a
    // requirement's signature reads the same here as it does in interface_implementations. this header
    // promises the two cannot disagree about whether a conformance is met, and this is that promise
    const AST::TypeSubstitution subst = AST::conformance_bindings(ct, interface, types).substitution;

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

    // **an associated type has no binding at an erased use site.** a vtable *can* be built - every
    // requirement still has an answering member - but `iterate() : Iter` has no static result type once
    // the value has forgotten which implementor it holds. that is an existential, and there is no opaque
    // type to give it. (the generic-instantiation refusal above is a frequent *consequence* of this, but
    // it is a fact about `from`; this one is a fact about the interface, which is why it is separate)
    const std::vector<AST::TypeParamDecl *> &associated =
        AST::interface_associated_types(interface.get_complex_type());

    if (!associated.empty()) {
        const AST::TypeParamDecl *assoc = associated.front();

        return fmt::format(
            "'{}' declares the associated type '{}', so it cannot be stored as a value - an erased value "
            "has forgotten which implementor it holds, and a requirement returning '{}' has no type "
            "without one. Use it as a constraint, or erase the '{}' itself.",
            interface.get_type_desciption(),
            assoc->name,
            assoc->name,
            assoc->constraint_spelling);
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
    const AST::ComplexType *ct,
    const AST::ValueType &interface,
    AST::TypeRegistry &types
)
{
    if (ct == nullptr || !interface.is_interface()) {
        return {};
    }

    const AST::TypeSubstitution subst = AST::conformance_bindings(ct, interface, types).substitution;
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
