#ifndef ASTARGUMENTFIT_H
#define ASTARGUMENTFIT_H

#pragma once

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

        // a numeric conversion that stays inside its family and cannot lose anything: a wider
        // integer of the same signedness, or a wider float. ranked above a plain conversion so
        // that `w(int64)` and `w(float64)` called with an int32 resolves to the integer one
        // instead of being ambiguous
        t_promotion,

        // any other primitive that reaches the parameter through TypeLowering::coerce_value - a
        // narrowing, a sign change, int to float. always possible, never free
        t_conversion,

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

        // a non-nullable borrow parameter takes the address of any place. a nullable `ptr<T>`
        // deliberately does not auto-borrow: taking an address is a decision the caller should be
        // able to see in the source (book/concept/pointers_and_refs_v2.md, "Passing to functions")
        if (to.is_pointer() && !to.is_nullable() && expr != nullptr && is_place_expression(*expr)) {
            const ValueType pointee = ValueType::make_mutable(to.pointee());
            if (ValueType::make_mutable(from) == pointee) {
                return ArgumentFit::t_borrow;
            }
        }

        // whatever is left between two primitives is a conversion TypeLowering::coerce_value
        // knows how to emit. a pointer, struct or class on either side has no such table
        if (from.is_primitive() && to.is_primitive()) {
            return is_value_preserving_promotion(from, to)
                ? ArgumentFit::t_promotion
                : ArgumentFit::t_conversion;
        }

        return ArgumentFit::t_none;
    }
};

#endif
