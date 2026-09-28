#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBuiltin.h>
#include <AST/ASTCopy.h>
#include <AST/ASTSimd.h>
#include <AST/ASTValueType.h>

#include "helpers.h"

using namespace AST;

TEST_CASE("A simd type is identified by element and length", "[types][simd]")
{
    const ValueType uint8 = EchoTests::prim(ValueTypePrimitive::t_uint8);
    const ValueType sixteen = ValueType::make_const_value(ValueTypePrimitive::t_usize, 16);
    const ValueType eight = ValueType::make_const_value(ValueTypePrimitive::t_usize, 8);

    const ValueType a = ValueType::make_simd(uint8, sixteen);
    const ValueType b = ValueType::make_simd(uint8, sixteen);
    const ValueType c = ValueType::make_simd(uint8, eight);

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE(a.is_simd());
    REQUIRE_FALSE(a.is_inline_array());
    REQUIRE(a.get_type_desciption() == "simd<uint8, 16>");
    REQUIRE(a.bound_simd_length() == 16);
    REQUIRE(c.bound_simd_length() == 8);
    REQUIRE(a.get_mangled_name() != c.get_mangled_name());
    REQUIRE(classify_copy(a) == CopyKind::t_bytes);
}

TEST_CASE("simd_shape_refusal admits a 16-byte integer vector and refuses the rest", "[types][simd]")
{
    const ValueType uint8 = EchoTests::prim(ValueTypePrimitive::t_uint8);
    const ValueType float64 = EchoTests::prim(ValueTypePrimitive::t_float64);
    const ValueType sixteen = ValueType::make_const_value(ValueTypePrimitive::t_usize, 16);
    const ValueType four = ValueType::make_const_value(ValueTypePrimitive::t_usize, 4);
    const ValueType three = ValueType::make_const_value(ValueTypePrimitive::t_usize, 3);

    REQUIRE(simd_shape_refusal(ValueType::make_simd(uint8, sixteen)).kind == SimdShapeKind::t_fine);
    REQUIRE(simd_shape_refusal(ValueType::make_simd(uint8, three)).kind == SimdShapeKind::t_refused);
    REQUIRE(simd_shape_refusal(ValueType::make_simd(float64, four)).kind == SimdShapeKind::t_refused);

    const ValueType bad = ValueType::make_simd(ValueType::make_void(), four);
    REQUIRE(simd_shape_refusal(ValueType::make_pointer(bad, true)).kind == SimdShapeKind::t_refused);
    REQUIRE(simd_shape_refusal(ValueType::make_inline_array(bad, four)).kind == SimdShapeKind::t_refused);
}

TEST_CASE("is_simd_builtin names the seven verbs", "[types][simd]")
{
    REQUIRE(is_simd_builtin(BuiltinKind::t_simd_splat));
    REQUIRE(is_simd_builtin(BuiltinKind::t_simd_load));
    REQUIRE(is_simd_builtin(BuiltinKind::t_simd_bitmask));
    REQUIRE_FALSE(is_simd_builtin(BuiltinKind::t_atomic_load));
    REQUIRE_FALSE(is_simd_builtin(BuiltinKind::t_take));
}

TEST_CASE("simd_crosses_c_refusal names a vector by value and admits a pointer to one", "[types][simd]")
{
    const ValueType uint8 = EchoTests::prim(ValueTypePrimitive::t_uint8);
    const ValueType sixteen = ValueType::make_const_value(ValueTypePrimitive::t_usize, 16);
    const ValueType four = ValueType::make_const_value(ValueTypePrimitive::t_usize, 4);
    const ValueType vec = ValueType::make_simd(uint8, sixteen);

    REQUIRE(simd_crosses_c_refusal(vec).has_value());
    REQUIRE_FALSE(simd_crosses_c_refusal(ValueType::make_pointer(vec, true)).has_value());
    REQUIRE(simd_crosses_c_refusal(ValueType::make_inline_array(vec, four)).has_value());
}
