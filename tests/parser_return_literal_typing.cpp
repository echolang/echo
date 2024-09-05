#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/FunctionDeclNode.h>
#include <AST/ReturnNode.h>
#include <AST/ScopeNode.h>
#include <AST/ExprNode.h>
#include <AST/VarDeclNode.h>

using namespace AST;

// a returned literal is typed where it is written, against the declared return type the parser
// carries on Context::return_type_ptr - the same hint a variable declaration gives its
// initializer. these assert that at the AST level, which the e2e goldens cannot distinguish: a
// float64 literal and an int32 literal converted at codegen print identically, but only the first
// one gets the literal's own bounds diagnostics

namespace {
    ValueType prim(ValueTypePrimitive p) { return ValueType(p); }

    // the return expression of the named function's body. the module is parsed twice (symbol pass
    // then full pass), so the bodyless symbol-pass node is skipped
    ExprNode *return_expr_of(Module &m, const std::string &name)
    {
        for (auto *fn : m.nodes.of_type<FunctionDeclNode>()) {
            if (fn->func_name() != name || fn->body == nullptr) {
                continue;
            }

            for (auto &child : fn->body->children) {
                if (child.has_type<ReturnNode>()) {
                    return child.get<ReturnNode>().expr;
                }
            }
        }

        return nullptr;
    }

    ValueType return_expr_type(Bundle &bundle, const std::string &name)
    {
        auto *expr = return_expr_of(bundle.modules.find_module("test"), name);
        REQUIRE(expr != nullptr);
        return expr->result_type();
    }
}

TEST_CASE("a returned literal is typed against the declared return type", "[Parser][Return]")
{
    SECTION("an integer literal becomes a float when the function returns one")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle("function f() : float64 { return 0; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(return_expr_type(*bundle, "f") == prim(ValueTypePrimitive::t_float64));
    }

    SECTION("the literal takes the declared width, not the guessed int32")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function narrow() : uint8 { return 200; }\n"
            "function wide() : int64 { return 1; }\n"
            "function sized() : usize { return 8; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(return_expr_type(*bundle, "narrow") == prim(ValueTypePrimitive::t_uint8));
        REQUIRE(return_expr_type(*bundle, "wide") == prim(ValueTypePrimitive::t_int64));
        REQUIRE(return_expr_type(*bundle, "sized") == prim(ValueTypePrimitive::t_usize));
    }

    SECTION("a float literal stays a float32 when that is what was declared")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle("function f() : float32 { return 0.25; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(return_expr_type(*bundle, "f") == prim(ValueTypePrimitive::t_float32));
    }

    SECTION("a bool literal returned from a bool function is left alone")
    {
        // this reported "the boolean literal 'true' cannot be implicitly converted to the
        // expected type 'bool'" until the bool branch compared the literal to its own type first
        auto bundle = EchoTests::tests_make_parsed_bundle("function f() : bool { return true; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(return_expr_type(*bundle, "f") == prim(ValueTypePrimitive::t_bool));
    }

    SECTION("an out-of-range literal is reported where it is written")
    {
        EchoTests::assert_code_emits_issue(
            "function f() : int8 { return 300; }\n",
            "Integer overflow: The literal '300' is too large for the integer type 'int8'. The maximum value is '127'.");
    }

    SECTION("a negative literal returned as unsigned is reported")
    {
        EchoTests::assert_code_emits_issue(
            "function f() : uint32 { return -1; }\n",
            "Invalid type conversion: The integer literal '-1' cannot be implicitly converted to an unsigned integer because it is negative.");
    }
}

TEST_CASE("the return type hint is only passed down when it can type a literal", "[Parser][Return]")
{
    SECTION("a type parameter is not a hint")
    {
        // T says nothing about a literal until it is substituted, and autocast_literal_int
        // reports a bogus "unexpected token" for anything that is not float, integer or bool.
        // the instance's return is fitted by the conversion table at codegen instead
        auto bundle = EchoTests::tests_make_parsed_bundle("function f<T>() : T { return 0; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(return_expr_type(*bundle, "f") == prim(ValueTypePrimitive::t_int32));
    }

    SECTION("a pointer return type is not a hint either")
    {
        // a pointer destination keeps the address rather than typing anything, and `null` is not
        // a literal the autocast helpers know - neither may be reported against
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function borrow(int32& $r) : int32& { return $r; }\n"
            "function nothing() : ptr<int32> { return null; }\n"
            "int32 $a = 1;\n"
            "ptr<int32> $p = borrow($a);\n"
            "ptr<int32> $q = nothing();\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("a void return type is not a hint")
    {
        // a bare `return;` used to trip an assertion in the expression parser
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function f(int32 $n) : void { if ($n < 0) { return; } echo $n; }\n"
            "f(1);\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(return_expr_of(bundle->modules.find_module("test"), "f") == nullptr);
    }
}

TEST_CASE("the return type hint is scoped to its own function body", "[Parser][Return]")
{
    // the guard restores the enclosing return type rather than clearing it, so a declaration
    // cannot leak its type onto what follows it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function first() : float64 { return 1; }\n"
        "function second() : uint8 { return 2; }\n"
        "$after = 3;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
    REQUIRE(return_expr_type(*bundle, "first") == prim(ValueTypePrimitive::t_float64));
    REQUIRE(return_expr_type(*bundle, "second") == prim(ValueTypePrimitive::t_uint8));

    // the file-scope declaration after both bodies still guesses on its own
    auto &m = bundle->modules.find_module("test");
    VarDeclNode *after = nullptr;
    for (auto *decl : m.nodes.of_type<VarDeclNode>()) {
        // name() drops the leading '$'
        if (decl->name() == "after") {
            after = decl;
        }
    }

    REQUIRE(after != nullptr);
    REQUIRE(after->type() == prim(ValueTypePrimitive::t_int32));
}
