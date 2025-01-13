#include "AST/ASTTypeUnify.h"

#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

bool AST::unify_type(const AST::ValueType &param, const AST::ValueType &arg, AST::TypeSubstitution &out, bool allow_decay)
{
    // generic inference decays a pointer argument to its pointee unless the parameter asks
    // for a pointer explicitly, so `box($p)` yields Box<int32> rather than Box<ptr<int32>>
    // (book/concept/pointers_and_refs_v2.md, "Pointers and generics"). stated over the
    // whole param rather than only a bare `T`, so Box<T> against ptr<Box<int32>> decays too
    // instead of silently binding nothing
    //
    // only at the top level though: asking for `ptr<T>` is how the doc says to opt out of
    // the decay, so everything below that match binds exactly
    if (allow_decay && !param.is_pointer() && arg.is_pointer()) {
        return unify_type(param, value_type_of(arg), out, allow_decay);
    }

    // pointer against pointer binds structurally, one level down
    if (param.is_pointer() && arg.is_pointer()) {
        return unify_type(param.pointee(), arg.pointee(), out, false);
    }

    // a bare type parameter binds directly to the argument type
    if (param.is_type_param()) {
        out.bind(param.get_type_param(), arg);
        return true;
    }

    // a generic application binds structurally, e.g. Box<T> against Box<int> binds T=int
    if (param.has_complex_type() && arg.has_complex_type()) {
        ComplexType *pct = param.get_complex_type();
        ComplexType *act = arg.get_complex_type();

        if (pct == nullptr || act == nullptr) {
            return false;
        }

        if (!pct->is_instantiated() || !act->is_instantiated()) {
            // two plain structs reconcile only by being the same type
            return pct == act;
        }

        if (pct->template_ref != act->template_ref
            || pct->instantiation_args.size() != act->instantiation_args.size()) {
            return false;
        }

        // exact, like the pointer descent above: `Box<int32&>` is a different layout
        // from `Box<int32>`, so binding T by reading the borrow away would pick the
        // wrong instance rather than a compatible one
        for (size_t i = 0; i < pct->instantiation_args.size(); i++) {
            if (!unify_type(pct->instantiation_args[i], act->instantiation_args[i], out, false)) {
                return false;
            }
        }

        return true;
    }

    // nothing was bound, and the parameter's shape is what decides whether that is a failure
    //
    // **still mentions a type parameter**: the shapes genuinely did not match. none of the arms above
    // could descend, so no substitution exists that makes this argument fit - `Box<T>&` against an
    // int32 binds nothing and never will. the candidate is out, which is also how a template stays
    // out of an overload set it has no business in
    //
    // **type-parameter free**: there was nothing to bind, so unification has no opinion. whether the
    // argument can *reach* the parameter is deliberately not asked here - that is AST::argument_fit's,
    // and the matcher asks it of every candidate, generic and concrete alike, once this has
    // substituted. this arm used to answer it too (is_implicitly_convertible, then a
    // primitive/primitive catch-all), so a generic candidate was filtered by one rule and then scored
    // by another, with nothing to notice when the two disagreed
    return !contains_type_param(param);
}

AST::InstantiationFit AST::can_instantiate(
    const AST::FunctionDeclNode *tmpl,
    const std::vector<AST::ValueType> &argument_types,
    AST::TypeSubstitution &out)
{
    if (tmpl->args.size() != argument_types.size()) {
        return InstantiationFit::t_no;
    }

    for (size_t i = 0; i < argument_types.size(); i++) {
        // an argument with no type yet cannot contradict the template, and cannot bind anything
        // either. it is the reason for the t_maybe answer below
        if (is_undetermined_type(argument_types[i])) {
            continue;
        }

        if (!tmpl->args[i]->has_type()) {
            continue;
        }

        if (!unify_type(tmpl->args[i]->type(), argument_types[i], out)) {
            return InstantiationFit::t_no;
        }
    }

    // a constraint is only violated by a binding that exists. an unbound parameter has not
    // failed its constraint, it simply has not been decided
    for (const auto *param : tmpl->type_parameters) {
        const auto *bound = out.lookup(param);

        if (bound == nullptr) {
            return InstantiationFit::t_maybe;
        }

        if (!param->allows(*bound)) {
            return InstantiationFit::t_no;
        }
    }

    return InstantiationFit::t_yes;
}
