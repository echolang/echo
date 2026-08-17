#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAtomics.h>
#include <AST/ASTBuiltin.h>
#include <AST/ASTValueType.h>

TEST_CASE("atomic_operand_refusal admits a word and refuses the rest", "[atomics]")
{
    using namespace AST;

    const ValueType i32{ValueTypePrimitive::t_int32};
    const ValueType u64{ValueTypePrimitive::t_uint64};
    const ValueType usize{ValueTypePrimitive::t_usize};
    const ValueType boolean{ValueTypePrimitive::t_bool};
    const ValueType f32{ValueTypePrimitive::t_float32};
    const ValueType f64{ValueTypePrimitive::t_float64};

    REQUIRE_FALSE(atomic_operand_refusal(i32, BuiltinKind::t_atomic_load).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(i32, BuiltinKind::t_atomic_add).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(u64, BuiltinKind::t_atomic_exchange).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(usize, BuiltinKind::t_atomic_sub).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(boolean, BuiltinKind::t_atomic_load).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(boolean, BuiltinKind::t_atomic_store).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(i32, BuiltinKind::t_atomic_fence).has_value());

    REQUIRE(atomic_operand_refusal(boolean, BuiltinKind::t_atomic_add).has_value());
    REQUIRE(atomic_operand_refusal(boolean, BuiltinKind::t_atomic_sub).has_value());
    REQUIRE(atomic_operand_refusal(f32, BuiltinKind::t_atomic_load).has_value());
    REQUIRE(atomic_operand_refusal(f64, BuiltinKind::t_atomic_add).has_value());

    const ValueType ptr = ValueType::make_pointer(i32, /*nullable=*/true);
    REQUIRE_FALSE(atomic_operand_refusal(ptr, BuiltinKind::t_atomic_load).has_value());
    REQUIRE_FALSE(atomic_operand_refusal(ptr, BuiltinKind::t_atomic_store).has_value());
    REQUIRE(atomic_operand_refusal(ptr, BuiltinKind::t_atomic_add).has_value());
    REQUIRE(atomic_operand_refusal(ptr, BuiltinKind::t_atomic_sub).has_value());
}

TEST_CASE("is_atomic_builtin names the seven verbs", "[atomics]")
{
    using namespace AST;

    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_load));
    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_store));
    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_add));
    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_sub));
    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_exchange));
    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_compare_exchange));
    REQUIRE(is_atomic_builtin(BuiltinKind::t_atomic_fence));
    REQUIRE_FALSE(is_atomic_builtin(BuiltinKind::t_take));
    REQUIRE_FALSE(is_atomic_builtin(BuiltinKind::t_exit));
}
