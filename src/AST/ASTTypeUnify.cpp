#include "AST/ASTTypeUnify.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTTypeParam.h"

bool AST::unify_type(const AST::ValueType &param, const AST::ValueType &arg, AST::TypeSubstitution &out, bool allow_decay, AST::UnifyPosition position)
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

    // a bare type parameter binds directly to the argument type, with the argument's `const` stripped
    // wherever that `const` describes a **place** rather than a type - AST::UnifyPosition owns which of
    // the two this is, and the two cases are worth keeping apart here because both are load-bearing
    //
    // on a level, `const` is a property of the *place* a value was read from, not of the value: `$key`
    // inside `f(const K& $key)` reads as a `const K`, and passing it on must bind `K` and not `const K`.
    // Without the strip one intent mints two instantiations - `map<const string, int32>` beside
    // `map<string, int32>` - and because ValueType equality is exact they are unrelated types, so the
    // receiver of the second call no longer converts to the first. Which is how it surfaced: a container
    // whose accessors take `const K&` could not call one from another
    //
    // the same rule AST::constraint_admits already applies when it compares ("compare with const stripped
    // so `const float` still matches `float`") and the same one AST::array_literal_type_for applies when it
    // mints from an element. stated here so inference agrees with both rather than being a third answer
    //
    // inside a type argument the strip is the opposite of harmless: `slice<T>` against a
    // `slice<const int32>` argument bound T=int32, so the substituted parameter came back `slice<int32>`
    // and AST::argument_fit scored t_none against the very argument that produced the binding - a refusal
    // whose diagnostic never says the word const. a read-only window is a *type*, and a parameter written
    // over it has to be able to name it
    //
    // a `const` the parameter's own level already states is consumed either way, so `slice<const T>`
    // against `slice<const int32>` still binds T=int32: the parameter said const, and what it is matching
    // is what is left
    if (param.is_type_param()) {
        const bool consumes_const = param.is_const() || position == UnifyPosition::t_level;

        out.bind(param.get_type_param(), consumes_const ? ValueType::make_mutable(arg) : arg);
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
