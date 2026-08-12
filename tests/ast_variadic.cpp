#include <catch2/catch_test_macros.hpp>

#include <AST/ASTValueType.h>
#include <AST/ASTVariadic.h>

// AST::variadic_promotion_of - C's default argument promotions, and the one place they are written.
//
// pinned as a table because nothing downstream will tell you it is wrong. `tests_eco/functions/
// variadic_args_extern` printed the right answer with the promotion removed entirely - LLVM's own
// AArch64 lowering promoted the variadic tail - so an end-to-end case cannot be relied on to catch a
// missing entry here. Clang emits these in its frontend and so does this compiler; that agreement is
// what this asserts

using namespace AST;

namespace
{
    ValueType prim(ValueTypePrimitive kind)
    {
        return ValueType(kind);
    }
}

TEST_CASE("A variadic argument is promoted the way C promotes one", "[AST][variadic]")
{
    // a float always widens - the single most common way to get this wrong, and the one C itself
    // requires
    REQUIRE( variadic_promotion_of(prim(ValueTypePrimitive::t_float32))
        == prim(ValueTypePrimitive::t_float64) );

    // anything narrower than a machine int becomes one, signedness preserved
    REQUIRE( variadic_promotion_of(prim(ValueTypePrimitive::t_bool))
        == prim(ValueTypePrimitive::t_int32) );
    REQUIRE( variadic_promotion_of(prim(ValueTypePrimitive::t_int8))
        == prim(ValueTypePrimitive::t_int32) );
    REQUIRE( variadic_promotion_of(prim(ValueTypePrimitive::t_int16))
        == prim(ValueTypePrimitive::t_int32) );
    REQUIRE( variadic_promotion_of(prim(ValueTypePrimitive::t_uint8))
        == prim(ValueTypePrimitive::t_uint32) );
    REQUIRE( variadic_promotion_of(prim(ValueTypePrimitive::t_uint16))
        == prim(ValueTypePrimitive::t_uint32) );

    // and everything already at least that wide is handed straight back, so the coercion the call
    // resolver inserts is a no-op rather than a redundant cast
    for (const auto kind : {
        ValueTypePrimitive::t_int32, ValueTypePrimitive::t_int64,
        ValueTypePrimitive::t_uint32, ValueTypePrimitive::t_uint64,
        ValueTypePrimitive::t_usize, ValueTypePrimitive::t_isize,
        ValueTypePrimitive::t_float64,
    }) {
        REQUIRE( variadic_promotion_of(prim(kind)) == prim(kind) );
    }

    // a pointer is a pointer on every platform this targets, so it is left alone too
    const ValueType address = ValueType::make_pointer(prim(ValueTypePrimitive::t_uint8), true);
    REQUIRE( variadic_promotion_of(address) == address );
}

// what may be *in* a tail: primitives and addresses, and nothing else. a struct's unpacking is
// platform specific, and an interface or a callable is two words with no C spelling at all
TEST_CASE("A variadic tail admits primitives and addresses only", "[AST][variadic]")
{
    REQUIRE_FALSE( variadic_argument_refusal(prim(ValueTypePrimitive::t_int32)).has_value() );
    REQUIRE_FALSE( variadic_argument_refusal(prim(ValueTypePrimitive::t_float64)).has_value() );
    REQUIRE_FALSE( variadic_argument_refusal(
        ValueType::make_pointer(prim(ValueTypePrimitive::t_uint8), true)).has_value() );

    // `void` is not a value, so it is refused ahead of the primitive arm rather than by it
    REQUIRE( variadic_argument_refusal(prim(ValueTypePrimitive::t_void)).has_value() );
}
