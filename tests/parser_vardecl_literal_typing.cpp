#include <catch2/catch_test_macros.hpp>

#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/ExprNode.h>
#include <AST/VarDeclNode.h>
#include <AST/AssignNode.h>

#include "helpers.h"

using namespace AST;

// a declaration's initializer and an assignment's value are typed against their destination, the
// same way a returned literal is (parser_return_literal_typing.cpp) - and only a destination that
// *can* type a literal counts. these assert at the AST level what the e2e goldens cannot see: an
// int32 literal fitted at codegen prints the same as one that was typed at the destination, but
// only the second carries the literal's own bounds diagnostics

namespace
{
    ValueType prim(ValueTypePrimitive p) { return ValueType(p); }

    // the module is parsed twice (symbol pass then full pass) and the monomorphizer clones a
    // template per instance, so several declarations share a name - the last one wins, which is
    // the full pass's node
    VarDeclNode *vardecl_of(Module &m, const std::string &name)
    {
        VarDeclNode *found = nullptr;
        for (auto *decl : m.nodes.of_type<VarDeclNode>()) {
            // name() drops the leading '$'
            if (decl->name() == name) {
                found = decl;
            }
        }
        return found;
    }

    ValueType init_expr_type(Bundle &bundle, const std::string &name)
    {
        auto *decl = vardecl_of(bundle.modules.find_module("test"), name);
        REQUIRE(decl != nullptr);
        REQUIRE(decl->init_expr != nullptr);
        return decl->init_expr->result_type();
    }

    // the value of the n-th assignment in the module, in parse order
    ValueType assign_value_type(Bundle &bundle, size_t index)
    {
        auto assigns = bundle.modules.find_module("test").nodes.of_type<AssignNode>();
        REQUIRE(assigns.size() > index);
        REQUIRE(assigns[index]->value_expr != nullptr);
        return assigns[index]->value_expr->result_type();
    }
}

TEST_CASE("a declared literal is typed against the declared type", "[Parser][VarDecl]")
{
    SECTION("an integer literal becomes a float when the slot is one")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle("float64 $x = 1;\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(init_expr_type(*bundle, "x") == prim(ValueTypePrimitive::t_float64));
    }

    SECTION("the literal takes the declared width, not the guessed int32")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "uint8 $narrow = 200;\n"
            "int64 $wide = 1;\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(init_expr_type(*bundle, "narrow") == prim(ValueTypePrimitive::t_uint8));
        REQUIRE(init_expr_type(*bundle, "wide") == prim(ValueTypePrimitive::t_int64));
    }

    SECTION("an out-of-range literal is reported where it is written")
    {
        EchoTests::assert_code_emits_issue(
            "int8 $x = 300;\n",
            "Integer overflow: The literal '300' is too large for the integer type 'int8'. The maximum value is '127'.");
    }
}

TEST_CASE("a destination only types a literal when it can", "[Parser][VarDecl]")
{
    SECTION("a type parameter is not a hint")
    {
        // T says nothing until it is substituted. every one of these was "Unexpected token
        // 'integer_literal' found" while a t_generic expected type still reached the autocast
        // helpers (todo/B11) - the instance's slot is fitted by the conversion table at codegen
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function decl<T>() : T { T $t = 42; return $t; }\n"
            "function poke<T>(ptr<T> $v) : void { $v = 42; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(init_expr_type(*bundle, "t") == prim(ValueTypePrimitive::t_int32));

        // `$v = 42` writes through the pointer, so the destination is the pointee - T again
        REQUIRE(assign_value_type(*bundle, 0) == prim(ValueTypePrimitive::t_int32));
    }

    SECTION("a bool literal against a type parameter is not a hint either")
    {
        // the bool branch reports a different issue than the int one, through the same hole
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function decl<T>() : T { T $t = true; return $t; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(init_expr_type(*bundle, "t") == prim(ValueTypePrimitive::t_bool));
    }

    SECTION("a pointer destination does not type the literals inside its initializer")
    {
        // the hint is threaded through every operand of the expression, so the `1` sees the
        // declared ptr<int32> - but there it is only the element offset, not the value
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "int32 $a = 1;\n"
            "ptr<int32> $p = &$a;\n"
            "ptr<int32> $q = $p:$ + 1;\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("a struct destination is rejected by the semantic pass, not at the literal")
    {
        // this used to be the parser's bogus "Unexpected token 'integer_literal' found" too. it is
        // the only reason the literal hint could not simply be dropped
        EchoTests::assert_code_emits_issue(
            "struct Point { int32 $x; int32 $y; }\n"
            "Point $p = 42;\n",
            "Invalid type conversion: cannot implicitly convert 'int32' to 'Point'");
    }

    SECTION("a concrete pointee still types a literal written through the pointer")
    {
        // the counterpart: an assignment's destination is the *pointee*, so `$p = 20` on a
        // ptr<uint8> narrows the literal at the write and keeps its bounds diagnostics
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "uint8 $a = 1;\n"
            "ptr<uint8> $p = &$a;\n"
            "$p = 20;\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
        REQUIRE(assign_value_type(*bundle, 0) == prim(ValueTypePrimitive::t_uint8));
    }
}
