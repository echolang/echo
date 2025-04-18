#ifndef ASTARGUMENTFIT_H
#define ASTARGUMENTFIT_H

#pragma once

#include "AST/ASTConformance.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"

namespace AST
{
    // how well an argument answers a parameter. declaration order is best to worst, and that
    // order *is* the overload ranking - AST::match_function compares these values directly, so
    // reordering the enum changes which overload a call resolves to
    enum class ArgumentFit
    {
        // the declared types are identical
        t_exact,

        // convertible without losing anything the type system tracks: a `T&` handed to a
        // `ptr<T>`, a `int32&` handed to a `const int32&`
        t_widening,

        // the parameter is a borrow and the argument is a place, so its address is taken
        // implicitly. see AST::CallResolver::coerce_arguments, which performs exactly this case
        t_borrow,

        // the mirror of it: the argument is a place holding a non-nullable borrow and the parameter
        // wants the value, so the read goes one level through. PointerAdjuster::as_value_for already
        // emits that deref - this rank is only the *scoring* of it, which was missing
        //
        // beside t_borrow rather than anywhere else because it is the same class of adjustment, and
        // the two can never compete for one argument: one needs a pointer parameter and a value
        // argument, the other the reverse
        t_read_through,

        // a numeric conversion that stays inside its family and cannot lose anything: a wider
        // integer of the same signedness, or a wider float. ranked above a plain conversion so
        // that `w(int64)` and `w(float64)` called with an int32 resolves to the integer one
        // instead of being ambiguous
        t_promotion,

        // any other primitive that reaches the parameter through TypeLowering::coerce_value - a
        // narrowing, a sign change, int to float. always possible, never free
        t_conversion,

        // a class handle reaching a parameter of an interface it conforms to - the value keeps its
        // object and loses its static type, gaining a vtable. **below every built-in conversion
        // above**, so an overload taking the concrete class always beats one taking the interface: the
        // widening is what lets a caller hand a `Circle` to a `Drawable` parameter without writing
        // anything, never a way to hide a better-matching overload. that is `t_declared_conversion`'s
        // reasoning one rank earlier, and above it because this one is the compiler's own rule while
        // that one is something a type declared about itself
        //
        // deliberately **not** folded into is_implicitly_convertible, which would rank it t_widening
        // and make erasing a static type look as free as `T&` -> `ptr<T>`
        t_interface_widening,

        // the argument's own type declared how to convert itself, `#[implicit]`. below every
        // built-in conversion above rather than beside them, because that is what makes an
        // overload taking the owning type always beat one taking the window: this is the
        // fallback that lets a caller take the cheap view without writing anything, never a
        // way to hide a better-matching overload. see AST::find_implicit_conversion
        t_declared_conversion,

        // the argument's type says nothing yet: a string literal, an unbound `null`, a member of
        // an incomplete struct, a mixed-operand binary expression, or anything still mentioning a
        // type parameter. neutral - it neither qualifies nor disqualifies a candidate
        t_undetermined,

        // no conversion exists
        t_none,
    };

    // a numeric conversion that keeps the value intact *and* stays in its own family: a wider
    // integer of the same signedness, or a wider float
    //
    // crossing families is deliberately not a promotion even when no value is lost - int32 into
    // float64 is exact, but it is a bigger step than int32 into int64, and every language that
    // ranks conversions at all ranks it lower. a sign change is not one either: the bit pattern
    // survives but the meaning does not
    //
    // spelled out here rather than reusing ValueType::will_fit_into, which answers a different
    // question - it accepts a bool into any numeric and rejects int32 into float64 as a side
    // effect of how it tests signedness, neither of which is what a ranking wants
    inline bool is_value_preserving_promotion(const ValueType &from, const ValueType &to)
    {
        const auto from_size = get_primitive_size(from.get_primitive_type());
        const auto to_size = get_primitive_size(to.get_primitive_type());

        if (from.is_integer_type() && to.is_integer_type()) {
            return from.is_signed_integer() == to.is_signed_integer() && from_size <= to_size;
        }

        if (from.is_floating_type() && to.is_floating_type()) {
            return from_size <= to_size;
        }

        return false;
    }

    // **which parameters auto-borrow** - the type-level half of the borrow rule, on its own because
    // it has a second reader that has no expression to ask about: AST::unify_type anticipates this
    // wrapping during inference, since the implicit address-of is inserted *after* a type argument is
    // bound (`bump<T>(T &$v)` called as `bump($a)` has to bind T=int32 from a bare value).
    //
    // a nullable `ptr<T>` deliberately does not auto-borrow: taking an address is a decision the
    // caller should be able to see in the source (book/concept/pointers_and_refs_v2.md, "Passing to
    // functions"). that exclusion is the whole reason this is one predicate rather than two spellings -
    // the inference and the coercion disagreeing about it would name an instance the coercion then
    // never produces
    inline bool parameter_auto_borrows(const ValueType &param)
    {
        return param.is_pointer() && !param.is_nullable();
    }

    // **the** rule for "does this argument answer this parameter" - the only implementation, with
    // three readers that each take a different amount of it:
    //
    //  - AST::match_function ranks with the whole ordering, which is what picks an overload;
    //  - AST::CallResolver's argument coercion reads only `t_borrow`, so a candidate the matcher
    //    accepted on the borrow arm is a candidate the coercion then actually wraps;
    //  - AST::TypeChecker reads only `!= t_none`, so a call it reports is a call resolution could
    //    not have chosen.
    //
    // it used to be three separate case analyses. the third was a hand-written copy in the type
    // checker whose pointer arm was plain is_implicitly_convertible, and the "first" was consulted
    // for concrete candidates while unify_type's tail filtered generic ones by yet another rule - so
    // a generic overload could be admitted by one and scored by the other with nothing to notice
    //
    // `expr` may be null when only the argument's type is known; the borrow rule needs the
    // expression, because only an expression can be a place. a caller that passes null therefore
    // declines the borrow arm, which is what a *cast* wants - a cast is not an address-of
    inline ArgumentFit argument_fit(const ValueType &from, const ExprNode *expr, const ValueType &to)
    {
        // no information. checked first so an undetermined argument can never be read as a
        // mismatch - the type checker and the monomorphizer's inference both already treat
        // unknown and void this way
        if (is_undetermined_type(from) || is_undetermined_type(to)) {
            return ArgumentFit::t_undetermined;
        }

        if (from == to) {
            return ArgumentFit::t_exact;
        }

        // before the borrow rule, so an argument that already fits a borrow parameter is not
        // wrapped in a second address-of. that ordering is load-bearing in
        // AST::CallResolver::coerce_arguments, where taking the address of a ptr<int32> for a
        // ptr<int32> parameter would build a ptr<ptr<int32>>
        if (is_implicitly_convertible(from, to)) {
            return ArgumentFit::t_widening;
        }

        // a borrow parameter takes the address of any place - which parameters those are is
        // parameter_auto_borrows, shared with the inference that has to predict this
        if (parameter_auto_borrows(to) && expr != nullptr && is_place_expression(*expr)) {
            const ValueType pointee = ValueType::make_mutable(to.pointee());
            if (ValueType::make_mutable(from) == pointee) {
                return ArgumentFit::t_borrow;
            }
        }

        // reading one level *through* a borrow to fill a value parameter, which is what
        // `take($b)` means for a `Point& $b` and a `Point` parameter. the deref is inserted
        // afterwards by PointerAdjuster::as_value_for, exactly as the borrow arm's address-of is
        // inserted by CallResolver - so both arms score a wrapping that a later pass performs
        //
        // AST::unify_type already decays a pointer argument to its pointee for a value parameter
        // (its `allow_decay` arm), so without this rank inference named an instance that scoring
        // then rejected. with one candidate the disagreement was invisible, because matching rule 2
        // wins without consulting types at all; with two it was a hard "no overload accepts these
        // arguments", so adding an unrelated overload broke a call that had always compiled
        //
        // **non-nullable only.** reading through a `ptr<T>` that may be null is an unchecked
        // dereference, and the one narrowing that does emit a check goes the other way
        // (book/concept/pointers_and_refs_v2.md, "Nullability"). a nullable argument stays t_none,
        // which is what it already was
        if (from.is_pointer() && !from.is_nullable() && !to.is_pointer()
            && expr != nullptr && is_place_expression(*expr)) {
            if (ValueType::make_mutable(value_type_of(from)) == ValueType::make_mutable(to)) {
                return ArgumentFit::t_read_through;
            }
        }

        // whatever is left between two primitives is a conversion TypeLowering::coerce_value
        // knows how to emit. a pointer, struct or class on either side has no such table
        if (from.is_primitive() && to.is_primitive()) {
            return is_value_preserving_promotion(from, to)
                ? ArgumentFit::t_promotion
                : ArgumentFit::t_conversion;
        }

        // a class handle widening to an interface it conforms to. asked of AST::conforms_to, the one
        // answer to that question, so the ranking and the lowering cannot disagree about which
        // conversions exist
        //
        // **a class only.** a struct's conformance is a compile-time contract - it has no runtime type
        // for a dispatch to read - so a struct argument answers t_none here and the type checker
        // reports it against the destination, which is where the reason belongs
        if (to.is_interface() && from.is_class() && conforms_to(from, to)) {
            return ArgumentFit::t_interface_widening;
        }

        // a value converts to another type when its own type declared how - a method marked
        // `#[implicit]`, found by AST::find_implicit_conversion. last, and its own rank, so that
        // CallResolver can tell this case apart from the primitive conversions above by the rank
        // alone rather than by asking the lookup a second time.
        //
        // a **place**, for the borrow arm's reason one rule up: the conversion is a call whose receiver is
        // `$this`, so it needs an address. materialising a temporary for a non-place argument is todo/A13
        // (its A13b half), and until that lands `f('literal')` against a view parameter reports an
        // ordinary mismatch rather than being lowered wrong
        if (expr != nullptr && is_place_expression(*expr) && find_implicit_conversion(from, to) != nullptr) {
            return ArgumentFit::t_declared_conversion;
        }

        return ArgumentFit::t_none;
    }
};

#endif
