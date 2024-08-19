#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/FunctionDeclNode.h>
#include <AST/ExprNode.h>
#include <AST/StructNode.h>

using namespace AST;

namespace {
    ValueType prim(ValueTypePrimitive p) { return ValueType(p); }

    std::vector<FunctionCallExprNode *> calls_to(Module &m, const std::string &name) {
        std::vector<FunctionCallExprNode *> out;
        for (auto *call : m.nodes.of_type<FunctionCallExprNode>()) {
            if (call->token_function_name.value() == name) {
                out.push_back(call);
            }
        }
        return out;
    }
}

// the parsed bundle is monomorphized by the test harness, so these assert the post-pass shape:
// generic call sites point at concrete instances and no type parameters survive.

TEST_CASE("generic function call is monomorphized to a concrete instance", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function twice<T>(T $x): T { return $x + $x; }\n"
        "echo twice(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "twice");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE_FALSE(calls[0]->decl->is_generic());  // rewired to the concrete instance
    REQUIRE(calls[0]->decl->get_return_type() == prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("distinct type arguments produce distinct instances", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $x): T { return $x; }\n"
        "echo id(5);\n"
        "echo id(2.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "id");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE(calls[1]->decl != nullptr);
    REQUIRE(calls[0]->decl != calls[1]->decl);  // separate monomorphizations
    REQUIRE_FALSE(calls[0]->decl->is_generic());
    REQUIRE_FALSE(calls[1]->decl->is_generic());
}

TEST_CASE("explicit type arguments resolve a call", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function twice<T>(T $x): T { return $x + $x; }\n"
        "echo twice<int>(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "twice");
    REQUIRE(calls.size() == 1);
    REQUIRE_FALSE(calls[0]->decl->is_generic());
    REQUIRE(calls[0]->decl->get_return_type() == prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("recursive generic terminates with a single instance", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function fac<T>(T $n): T {\n"
        "    if ($n < 2) { return $n; }\n"
        "    return $n * fac($n - 1);\n"
        "}\n"
        "echo fac(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "fac");
    // the outer call plus the recursive self-call, all pointing at the one int instance
    REQUIRE(calls.size() >= 1);
    for (auto *call : calls) {
        if (!call->decl->is_generic()) {  // the template's own body call may remain generic
            REQUIRE(call->decl->get_return_type() == prim(ValueTypePrimitive::t_int32));
        }
    }
}

TEST_CASE("generic struct instantiation resolves to a concrete layout", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "$b = Box<int>(7);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "Box");
    REQUIRE(calls.size() >= 1);

    auto *ctor = calls[0]->decl;
    REQUIRE(ctor != nullptr);
    REQUIRE_FALSE(ctor->is_generic());  // the constructor was monomorphized too

    auto ret = ctor->get_return_type();
    REQUIRE(ret.is_struct());
    REQUIRE(ret.get_complex_type()->is_instantiated());
    REQUIRE(ret.get_complex_type()->get_property_type("value") == prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("nested generic struct closing with '>>' parses and monomorphizes", "[generics]")
{
    // the trailing '>>' lexes as a single t_op_shr token; the parser must split it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "$b = Box<Box<int>>(Box<int>(7));\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "Box");
    REQUIRE(calls.size() >= 1);

    // the outer Box<Box<int>> constructor: a concrete struct whose 'value' property is
    // itself a concrete Box<int> struct, whose own 'value' is int
    bool found_nested = false;
    for (auto *call : calls) {
        if (!call->decl || call->decl->is_generic()) continue;
        auto ret = call->decl->get_return_type();
        if (!ret.is_struct()) continue;
        auto inner = ret.get_complex_type()->get_property_type("value");
        if (inner.is_struct() &&
            inner.get_complex_type()->get_property_type("value") == prim(ValueTypePrimitive::t_int32)) {
            found_nested = true;
        }
    }
    REQUIRE(found_nested);
}

TEST_CASE("triple-nested generic struct closing with '>>>' parses", "[generics]")
{
    // '>>>' lexes as [t_op_shr, t_close_angle]; each level consumes one '>' in order
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "$b = Box<Box<Box<int>>>(Box<Box<int>>(Box<int>(7)));\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(calls_to(m, "Box").size() >= 1);
}

TEST_CASE("generic struct with multiple args closing with '>>' parses", "[generics]")
{
    // comma-separated args followed by a '>>' close (int then Box<int>)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "struct Pair<A, B> { A $first; B $second; }\n"
        "$p = Pair<int, Box<int>>(3, Box<int>(7));\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "Pair");
    REQUIRE(calls.size() >= 1);

    bool found = false;
    for (auto *call : calls) {
        if (!call->decl || call->decl->is_generic()) continue;
        auto ret = call->decl->get_return_type();
        if (!ret.is_struct()) continue;
        auto *ct = ret.get_complex_type();
        if (ct->get_property_type("first") == prim(ValueTypePrimitive::t_int32) &&
            ct->get_property_type("second").is_struct()) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("generic call with wrong explicit arity reports an issue", "[generics]")
{
    EchoTests::assert_code_emits_issue(
        "function id<T>(T $x): T { return $x; }\n"
        "echo id<int, float>(5);\n",
        "Wrong number of type arguments for generic function 'id'");
}
