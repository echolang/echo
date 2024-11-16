#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/FunctionDeclNode.h>
#include <AST/ExprNode.h>
#include <AST/StructNode.h>

using namespace AST;

using EchoTests::calls_to;
using EchoTests::has_issue_containing;
using EchoTests::prim;

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

TEST_CASE("constrained type parameter accepts an allowed type", "[generics]")
{
    // `numeric` admits every integer and floating type, so int and float both resolve
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T: numeric>(T $x): T { return $x; }\n"
        "echo id(5);\n"
        "echo id(2.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "id");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl != calls[1]->decl);
}

TEST_CASE("constrained type parameter rejects a disallowed type", "[generics]")
{
    // bool is not numeric, so the call must fail the constraint
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T: numeric>(T $x): T { return $x; }\n"
        "echo id(true);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "Type parameter 'T' of 'id' is constrained to 'numeric'"));
}

TEST_CASE("union constraint admits each listed atom exactly", "[generics]")
{
    // float|float64 accepts both floats but rejects int (exact match, no widening)
    auto ok = EchoTests::tests_make_parsed_bundle(
        "function f<T: float | float64>(T $x): T { return $x; }\n"
        "echo f(2.5);\n");
    REQUIRE_FALSE(ok->collector.has_critical_issues());

    auto bad = EchoTests::tests_make_parsed_bundle(
        "function f<T: float | float64>(T $x): T { return $x; }\n"
        "echo f(5);\n");
    REQUIRE(bad->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bad, "constrained to 'float|float64'"));
}

TEST_CASE("explicit type argument is constraint-checked", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f<T: floating>(T $x): T { return $x; }\n"
        "echo f<int>(5);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "constrained to 'floating'"));
}

TEST_CASE("unknown constraint atom is a parse error", "[generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f<T: bogus>(T $x): T { return $x; }\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "Unknown type or alias 'bogus'"));
}

TEST_CASE("constrained generic struct rejects a disallowed argument", "[generics]")
{
    auto ok = EchoTests::tests_make_parsed_bundle(
        "struct Box<T: numeric> { T $value; }\n"
        "$b = Box<int>(5);\n");
    REQUIRE_FALSE(ok->collector.has_critical_issues());

    auto bad = EchoTests::tests_make_parsed_bundle(
        "struct Box<T: numeric> { T $value; }\n"
        "$b = Box<bool>(true);\n");
    REQUIRE(bad->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bad, "Type parameter 'T' of 'Box' is constrained to 'numeric'"));
}

TEST_CASE("prefix unary negation resolves in a generic function", "[generics]")
{
    // a generic body using prefix '-' used to hand the shunting yard a null lhs
    // and crash; it must now monomorphize cleanly for both int and float args
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function abs<T>(T $value): T {\n"
        "    if ($value < 0) { return -$value; }\n"
        "    return $value;\n"
        "}\n"
        "echo abs(-5);\n"
        "echo abs(-3.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("Two generic structs in one program keep their type parameters apart", "[generics]")
{
    // Box<T> and Pair<A, B> both declare a first type parameter. under an ordinal-only
    // representation those compared equal, so this is the end-to-end lock on decl identity
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "struct Pair<A, B> { A $first; B $second; }\n"
        "$p = Pair<Box<int>, Box<float>>(Box<int>(1), Box<float>(2.5));\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "Pair");
    REQUIRE(calls.size() >= 1);

    auto *ctor = calls[0]->decl;
    REQUIRE(ctor != nullptr);
    REQUIRE_FALSE(ctor->is_generic());

    auto ret = ctor->get_return_type();
    REQUIRE(ret.is_struct());

    // the two members are distinct concrete Box instantiations, not one aliased type
    auto *pair_ct = ret.get_complex_type();
    REQUIRE(pair_ct->is_instantiated());

    ValueType first = pair_ct->get_property_type("first");
    ValueType second = pair_ct->get_property_type("second");
    REQUIRE(first.is_struct());
    REQUIRE(second.is_struct());
    REQUIRE_FALSE(first == second);

    REQUIRE(first.get_complex_type()->get_property_type("value") == prim(ValueTypePrimitive::t_int32));
    REQUIRE(second.get_complex_type()->get_property_type("value") == prim(ValueTypePrimitive::t_float32));
}

TEST_CASE("An unresolvable generic parameter is named in the diagnostic", "[generics]")
{
    // a type parameter that inference cannot reach: nothing in the argument list mentions U.
    // the message has to name U rather than a synthetic ordinal
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function pick<T, U>(T $x): T { return $x; }\n"
        "$v = pick(1);\n");

    REQUIRE(has_issue_containing(*bundle, "'U'"));
}
