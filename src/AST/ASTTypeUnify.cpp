#include "AST/ASTTypeUnify.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTTypeParam.h"

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

    // the mirror of that rule, and the reason a generic mutator can be written at all: a
    // non-nullable borrow parameter is filled by taking the address of a place argument
    // (AST::argument_fit's t_borrow), and that wrapping happens *after* inference - the call site
    // still reads as a bare value here. so bind through the borrow, which is exactly what the
    // implicit address-of will produce: `bump<T>(T &$v)` called as `bump($a)` binds T=int32
    //
    // which parameters those are is not decided here: AST::parameter_auto_borrows is the one
    // spelling, so this cannot come to a different answer than the coercion it is anticipating. a
    // nullable `ptr<T>` is excluded there, and excluded here by the same call
    //
    // top level only, like the decay it mirrors, and for the same reason: below a structural match
    // `allow_decay` is false, or `ptr<T>` against a `ptr<int32>` argument would bind T by two routes
    if (allow_decay && parameter_auto_borrows(param) && !arg.is_pointer()) {
        return unify_type(param.pointee(), arg, out, false);
    }

    // pointer against pointer binds structurally, one level down
    if (param.is_pointer() && arg.is_pointer()) {
        return unify_type(param.pointee(), arg.pointee(), out, false);
    }

    // and weak against weak, the same way: `cache<T>(weak<T> $w)` called with a `weak<Node>` binds
    // T=Node. `allow_decay` off below it for the pointer arm's reason
    //
    // note there is deliberately no arm decaying a `weak<Foo>` argument to `Foo` for a bare `T`
    // parameter, the way the top of this function decays a pointer. a pointer is *transparent* - reading
    // one yields its pointee, so inference following that read is inference agreeing with codegen. a
    // weak is not: reading it is refused, and binding T=Foo would name an instance whose body cannot be
    // handed what the call site actually has
    if (param.is_weak() && arg.is_weak()) {
        return unify_type(param.weak_target(), arg.weak_target(), out, false);
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
