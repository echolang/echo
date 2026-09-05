#include <catch2/catch_test_macros.hpp>

#include <AST/ASTCast.h>
#include <AST/ASTModule.h>
#include <AST/ASTValueType.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/LiteralValueNode.h>
#include <AST/TemporaryBindExprNode.h>
#include <AST/TypeCastNode.h>
#include <AST/TypeNode.h>

#include "helpers.h"

#include <vector>

// AST::cast_plan_for - the sole answer to "what does this written `$x as T` mean"

using namespace AST;

namespace
{
    ValueType prim(ValueTypePrimitive p)
    {
        return ValueType(p);
    }

    ValueType ptr_to(ValueType pointee)
    {
        return ValueType::make_pointer(pointee, true);
    }

    ValueType ref_to(ValueType pointee)
    {
        return ValueType::make_pointer(pointee, false);
    }

    // TypeCastNode::result_type is cast_to, so an implicit one stands in for a value of that
    // type. the built-in arms never ask implicit_conversion_for, so a dummy inner is enough
    TypeCastNode &typed(Module &tm, const ValueType &type, ExprNode *inner)
    {
        return tm.nodes.emplace_back<TypeCastNode>(type, inner, true);
    }
};

TEST_CASE("cast_plan_for is identity when only top-level const differs", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);

    const CastLookup lookup = cast_plan_for(lit, prim(ValueTypePrimitive::t_int32));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_identity);
}

TEST_CASE("a primitive-to-primitive cast is numeric", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);

    const CastLookup lookup = cast_plan_for(lit, prim(ValueTypePrimitive::t_float64));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_numeric);
}

TEST_CASE("a narrowing primitive cast is still numeric", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);
    const ValueType i32 = prim(ValueTypePrimitive::t_int32);
    const ValueType i64 = prim(ValueTypePrimitive::t_int64);

    const CastLookup lookup = cast_plan_for(typed(tm, i64, &lit), i32);
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_numeric);
}

TEST_CASE("a pointer-to-pointer cast is a pointer reinterpret", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);
    const ValueType i32 = prim(ValueTypePrimitive::t_int32);

    const CastLookup lookup = cast_plan_for(
        typed(tm, ptr_to(i32), &lit),
        ptr_to(prim(ValueTypePrimitive::t_uint8)));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_pointer);
}

TEST_CASE("a borrow promotion is a pointer cast", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);
    const ValueType i32 = prim(ValueTypePrimitive::t_int32);

    const CastLookup lookup = cast_plan_for(typed(tm, ptr_to(i32), &lit), ref_to(i32));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_pointer);
}

TEST_CASE("a pointer to a C function pointer is a function-pointer reinterpret", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);
    const ValueType i32 = prim(ValueTypePrimitive::t_int32);
    const ValueType cfn = ValueType::make_c_function(i32, { i32 });

    const CastLookup lookup = cast_plan_for(
        typed(tm, ptr_to(prim(ValueTypePrimitive::t_uint8)), &lit), cfn);
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_function_pointer);
}

TEST_CASE("a C function pointer to a pointer is a function-pointer reinterpret", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);
    const ValueType i32 = prim(ValueTypePrimitive::t_int32);
    const ValueType cfn = ValueType::make_c_function(i32, { i32 });

    const CastLookup lookup = cast_plan_for(
        typed(tm, cfn, &lit), ptr_to(prim(ValueTypePrimitive::t_uint8)));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_function_pointer);
}

TEST_CASE("an undetermined operand waits rather than refusing", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);

    const CastLookup lookup = cast_plan_for(typed(tm, ValueType::make_unknown(), &lit), prim(ValueTypePrimitive::t_int32));
    REQUIRE(lookup.result == CastLookup::Result::t_pending);
}

TEST_CASE("void is refused, not pending", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);

    const CastLookup lookup = cast_plan_for(lit, ValueType::void_type());
    REQUIRE(lookup.result == CastLookup::Result::t_refused);
    REQUIRE(lookup.refusal.find("cannot be read as") != std::string::npos);
}

TEST_CASE("an unresolved call as a destination waits rather than refusing", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("f");
    auto &call = tm.nodes.emplace_back<FunctionCallExprNode>(
        tm.tokens[0], std::vector<ExprNode *>{});

    const CastLookup lookup = cast_plan_for(call, prim(ValueTypePrimitive::t_int32));
    REQUIRE(lookup.result == CastLookup::Result::t_pending);
}

TEST_CASE("a settled void call as a destination is refused, naming the call", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("nothing");
    auto &decl = tm.nodes.emplace_back<FunctionDeclNode>(tm.tokens[0]);
    decl.return_type = &tm.nodes.emplace_back<TypeNode>(ValueType::void_type());
    auto &call = tm.nodes.emplace_back<FunctionCallExprNode>(
        tm.tokens[0], std::vector<ExprNode *>{});
    call.decl = &decl;

    const CastLookup lookup = cast_plan_for(call, prim(ValueTypePrimitive::t_int32));
    REQUIRE(lookup.result == CastLookup::Result::t_refused);
    REQUIRE(lookup.refusal.find("nothing()") != std::string::npos);
    REQUIRE(lookup.refusal.find("produces no value") != std::string::npos);
}

TEST_CASE("a TemporaryBind around a void call is refused, naming the call", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("bump");
    auto &decl = tm.nodes.emplace_back<FunctionDeclNode>(tm.tokens[0]);
    decl.return_type = &tm.nodes.emplace_back<TypeNode>(ValueType::void_type());
    auto &call = tm.nodes.emplace_back<FunctionCallExprNode>(
        tm.tokens[0], std::vector<ExprNode *>{});
    call.decl = &decl;
    auto &bind = tm.nodes.emplace_back<TemporaryBindExprNode>(&call, tm.tokens[0]);

    const CastLookup lookup = cast_plan_for(bind, prim(ValueTypePrimitive::t_int32));
    REQUIRE(lookup.result == CastLookup::Result::t_refused);
    REQUIRE(lookup.refusal.find("bump()") != std::string::npos);
}

TEST_CASE("a never-returning operand is identity, not a void refusal", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("die");
    auto &decl = tm.nodes.emplace_back<FunctionDeclNode>(tm.tokens[0]);
    decl.return_type = &tm.nodes.emplace_back<TypeNode>(ValueType::void_type());
    decl.builtin = "die";
    auto &call = tm.nodes.emplace_back<FunctionCallExprNode>(
        tm.tokens[0], std::vector<ExprNode *>{});
    call.decl = &decl;

    const CastLookup lookup = cast_plan_for(call, prim(ValueTypePrimitive::t_int32));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_identity);
}

TEST_CASE("unrelated primitives still convert", "[cast]")
{
    auto tm = EchoTests::tests_make_tokenized_module("0");
    auto &lit = tm.nodes.emplace_back<LiteralIntExprNode>(tm.tokens[0]);

    const CastLookup lookup = cast_plan_for(lit, prim(ValueTypePrimitive::t_bool));
    REQUIRE(lookup.result == CastLookup::Result::t_ok);
    REQUIRE(lookup.plan.kind == CastKind::t_numeric);
}
