#include <catch2/catch_test_macros.hpp>

#include <AST/ASTConstructForward.h>
#include <AST/FunctionDeclNode.h>

#include "helpers.h"

using namespace AST;

namespace
{
    FunctionDeclNode *function_named(AST::Module &module, const std::string &name)
    {
        for (auto *fn : module.nodes.of_type<FunctionDeclNode>()) {
            if (fn != nullptr && fn->func_name() == name && !fn->is_instantiated()) {
                return fn;
            }
        }

        return nullptr;
    }
}

TEST_CASE("construct_param_of reads the last user parameter", "[AST][construct]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Pair { int32 $a; int32 $b; }\n"
        "function make<T>(construct T $value) : T { return $value; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *make = function_named(m, "make");
    REQUIRE(make != nullptr);

    const VarDeclNode *param = construct_param_of(*make);
    REQUIRE(param != nullptr);
    REQUIRE(param->constructs_in_place);
    REQUIRE(param->name() == "value");
    REQUIRE_FALSE(construct_refusal(*make).has_value());
}

TEST_CASE("construct_refusal names a borrow, an optional, and a non-last slot", "[AST][construct]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function a(construct int32& $v) : int32 { return $v; }\n"
        "function b(construct int32? $v) : int32? { return $v; }\n"
        "function c(construct int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    REQUIRE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(construct_refusal(*function_named(m, "a")).has_value());
    REQUIRE(construct_refusal(*function_named(m, "b")).has_value());
    REQUIRE(*construct_refusal(*function_named(m, "c"))
        == "'construct' can only sit on the last parameter: remaining call-site arguments build this one.");
}

TEST_CASE("construct_gather_start is the extra-args slice", "[AST][construct]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Pair { int32 $a; int32 $b; }\n"
        "function make<T>(construct T $value) : T { return $value; }\n"
        "function tag<T>(int32 $x, construct T $value) : int32 { return $x; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *make = function_named(m, "make");
    auto *tag = function_named(m, "tag");
    REQUIRE(make != nullptr);
    REQUIRE(tag != nullptr);

    REQUIRE(*construct_gather_start(*make, 2) == 0);
    REQUIRE(*construct_gather_start(*make, 1) == 0);
    REQUIRE_FALSE(construct_gather_start(*make, 0).has_value());
    REQUIRE(*construct_gather_start(*tag, 3) == 1);
    REQUIRE(*construct_gather_start(*tag, 2) == 1);
    REQUIRE_FALSE(construct_gather_start(*tag, 1).has_value());
}

TEST_CASE("construct as a type name is still a type", "[AST][construct]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct construct { int32 $v; }\n"
        "function unwrap(construct $o) : int32 { return $o->v; }\n"
        "echo unwrap(construct(4));\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *unwrap = function_named(bundle->modules.find_module("test"), "unwrap");
    REQUIRE(unwrap != nullptr);
    REQUIRE(construct_param_of(*unwrap) == nullptr);
}
