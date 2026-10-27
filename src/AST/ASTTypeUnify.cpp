#include "AST/ASTTypeUnify.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTTypeParam.h"

bool AST::unify_type(const AST::ValueType &param, const AST::ValueType &arg, AST::TypeSubstitution &out, bool allow_decay, AST::UnifyPosition position)
{
    // generic inference decays a pointer argument to its pointee unless the parameter asks
    // for a pointer explicitly, so `box($p)` yields Box<int32> rather than Box<ptr<int32>>.
    // stated over the whole param rather than only a bare `T`, so Box<T> against
    // ptr<Box<int32>> decays too instead of silently binding nothing
    //
    // only at the top level though: asking for `ptr<T>` is how to opt out of the decay, so
    // everything below that match binds exactly
    if (allow_decay && !param.is_pointer() && arg.is_pointer()) {
        return unify_type(param, value_type_of(arg), out, allow_decay, position);
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
        return unify_type(param.pointee(), arg, out, false, position);
    }

    // pointer against pointer binds structurally, one level down
    if (param.is_pointer() && arg.is_pointer()) {
        return unify_type(param.pointee(), arg.pointee(), out, false, position);
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
        return unify_type(param.weak_target(), arg.weak_target(), out, false, position);
    }

    // a bare type parameter binds to the argument. UnifyPosition says whether to strip `const`:
    // on a level it describes the place and is stripped (`const K&` binds K, not const K);
    // inside a type argument it is identity (`slice<T>` vs `slice<const int32>` binds T=const int32)
    if (param.is_type_param()) {
        const TypeParamDecl *decl = param.get_type_param();

        if (decl->is_value_param()) {
            if (arg.is_const_value()) {
                if (!ValueType(arg.const_value_primitive()).is_integer_type()
                    || !decl->value_type.is_integer_type()) {
                    return false;
                }

                out.bind(decl, retype_const_generic(*decl, arg));
                return true;
            }

            if (arg.is_type_param() && arg.get_type_param()->is_value_param()) {
                out.bind(decl, arg);
                return true;
            }

            return false;
        }

        if (arg.is_const_value()) {
            return false;
        }

        if (arg.is_type_param() && arg.get_type_param()->is_value_param()) {
            return false;
        }

        const bool consumes_const = param.is_const() || position == UnifyPosition::t_level;
        const ValueType incoming = consumes_const ? ValueType::make_mutable(arg) : arg;

        // a later argument must not replace a concrete binding. `pick($int32, $int64)` used
        // to instantiate `pick<int32>` and silently narrow the int64
        if (const ValueType *existing = out.lookup(decl)) {
            if (!contains_type_param(*existing)) {
                return *existing == incoming;
            }
        }

        out.bind(decl, incoming);
        return true;
    }

    if (param.is_const_value() && arg.is_const_value()) {
        return param == arg;
    }

    if (param.is_inline_array() && arg.is_inline_array()) {
        return unify_type(param.array_element(), arg.array_element(), out, false, UnifyPosition::t_type_argument)
            && unify_type(param.array_length(), arg.array_length(), out, false, UnifyPosition::t_type_argument);
    }

    if (param.is_simd() && arg.is_simd()) {
        return unify_type(param.simd_element(), arg.simd_element(), out, false, UnifyPosition::t_type_argument)
            && unify_type(param.simd_length(), arg.simd_length(), out, false, UnifyPosition::t_type_argument);
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
        //
        // and t_type_argument for the same reason one level up: `Box<const int32>` is a different
        // layout too, so a `const` reached from here belongs to the instantiation and not to a place
        for (size_t i = 0; i < pct->instantiation_args.size(); i++) {
            if (!unify_type(
                    pct->instantiation_args[i], act->instantiation_args[i], out, false,
                    UnifyPosition::t_type_argument)) {
                return false;
            }
        }

        return true;
    }

    // two signatures of the same kind bind structurally: `extern function<T(T)>` against
    // `extern function<int32(int32)>` binds T. kinds must agree so a C function pointer and
    // an Echo callable stay distinct. `allow_decay` off below, for the pointer arm's reason
    if (param.has_signature() && arg.has_signature()) {
        if (param.is_c_function() != arg.is_c_function()) {
            return false;
        }

        const CallableSignature &ps = param.signature();
        const CallableSignature &as = arg.signature();

        if (ps.parameter_types.size() != as.parameter_types.size()) {
            return false;
        }

        if (!unify_type(ps.return_type, as.return_type, out, false, position)) {
            return false;
        }

        for (size_t i = 0; i < ps.parameter_types.size(); i++) {
            if (!unify_type(ps.parameter_types[i], as.parameter_types[i], out, false, position)) {
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
    // substituted. answering it here too (is_implicitly_convertible, then a primitive/primitive
    // catch-all) would filter a generic candidate by one rule and then score it by another, with
    // nothing to notice when the two disagreed
    return !contains_type_param(param);
}
