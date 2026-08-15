#include <catch2/catch_test_macros.hpp>

#include <AST/ASTCFunction.h>
#include <AST/ASTCopy.h>
#include <AST/ASTDestruction.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::prim;

namespace
{
    ValueType int32_type()
    {
        return prim(ValueTypePrimitive::t_int32);
    }
}

TEST_CASE("a C function-pointer type written twice is one type", "[cfn]")
{
    const ValueType a = ValueType::make_c_function(int32_type(), { int32_type() });
    const ValueType b = ValueType::make_c_function(int32_type(), { int32_type() });

    REQUIRE(a == b);
    REQUIRE(std::hash<ValueType>{}(a) == std::hash<ValueType>{}(b));
}

TEST_CASE("a C function pointer is not a callable", "[cfn]")
{
    const ValueType cfn = ValueType::make_c_function(int32_type(), { int32_type() });
    const ValueType fn = ValueType::make_callable(int32_type(), { int32_type() });

    REQUIRE_FALSE(cfn == fn);
    REQUIRE(cfn.is_c_function());
    REQUIRE_FALSE(cfn.is_callable());
    REQUIRE(cfn.has_signature());
    REQUIRE(fn.has_signature());
    REQUIRE_FALSE(int32_type().has_signature());
}

TEST_CASE("two different C function-pointer types get different mangled names", "[cfn]")
{
    const ValueType a = ValueType::make_c_function(int32_type(), { int32_type() });
    const ValueType b = ValueType::make_c_function(ValueType::make_void(), {});

    REQUIRE(a.get_mangled_name() != b.get_mangled_name());
    REQUIRE(a.get_mangled_name()
        == ValueType::make_c_function(int32_type(), { int32_type() }).get_mangled_name());
    REQUIRE(a.get_mangled_name()
        != ValueType::make_callable(int32_type(), { int32_type() }).get_mangled_name());
}

TEST_CASE("a C function pointer renders as it is written", "[cfn]")
{
    REQUIRE(
        ValueType::make_c_function(int32_type(), { int32_type() }).get_type_desciption()
        == "extern function<int32(int32)>");
}

TEST_CASE("a C function pointer copies as bytes and needs no destruction", "[cfn]")
{
    const ValueType type = ValueType::make_c_function(int32_type(), { int32_type() });

    REQUIRE(classify_copy(type) == CopyKind::t_bytes);
    REQUIRE_FALSE(needs_destruction(type));
}

TEST_CASE("the C function-pointer type parses in every position", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add1(int32 $x) : int32 { return $x + 1; }\n"
        "struct Holder { extern function<int32(int32)> $op; }\n"
        "function apply(extern function<int32(int32)> $f, int32 $v) : int32 { return $f($v); }\n"
        "function pick() : extern function<int32(int32)> { return &add1; }\n"
        "extern function<int32(int32)> $local = &add1;\n"
        "echo apply($local, 1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    auto applies = decls_named(m, "apply");
    REQUIRE(applies.size() == 1);
    REQUIRE(applies[0]->parameter_type(0) == ValueType::make_c_function(int32_type(), { int32_type() }));

    auto picks = decls_named(m, "pick");
    REQUIRE(picks.size() == 1);
    REQUIRE(picks[0]->get_return_type() == ValueType::make_c_function(int32_type(), { int32_type() }));
}

TEST_CASE("a C function pointer is not implicitly convertible to a different signature", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add1(int32 $x) : int32 { return $x + 1; }\n"
        "function apply(extern function<int32(int32)> $f) : int32 { return $f(1); }\n"
        "extern function<float64(float64)> $g = null;\n"
        "echo apply($g);\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

TEST_CASE("&name of a unique function resolves immediately", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add1(int32 $x) : int32 { return $x + 1; }\n"
        "extern function<int32(int32)> $f = &add1;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto refs = m.nodes.of_type<FunctionRefExprNode>();
    REQUIRE(refs.size() == 1);
    REQUIRE(refs[0]->resolved);
    REQUIRE(refs[0]->decl != nullptr);
    REQUIRE(refs[0]->result_type() == ValueType::make_c_function(int32_type(), { int32_type() }));
}

TEST_CASE("an ambiguous &name without a destination is refused", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add(int32 $x) : int32 { return $x; }\n"
        "function add(float64 $x) : float64 { return $x; }\n"
        "$f = &add;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "ambiguous"));
}

TEST_CASE("& of a method is refused", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { function origin() : int32 { return 0; } }\n"
        "extern function<int32()> $f = &Point::origin;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "receiver"));
}

TEST_CASE("a struct by value in a C function-pointer signature is refused", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { int32 $x; }\n"
        "extern function<void(Point)> $f = null;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "by value"));
}

TEST_CASE("a closure at a C function-pointer destination is refused", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern function<int32(int32)> $f = function(int32 $x) : int32 { return $x; };\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "&name"));
}

TEST_CASE("an overloaded &name at a return destination binds", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add(int32 $x) : int32 { return $x; }\n"
        "function add(float64 $x) : float64 { return $x; }\n"
        "function pick() : extern function<int32(int32)> { return &add; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto refs = m.nodes.of_type<FunctionRefExprNode>();
    REQUIRE(refs.size() == 1);
    REQUIRE(refs[0]->resolved);
    REQUIRE(refs[0]->decl != nullptr);
    REQUIRE(refs[0]->result_type() == ValueType::make_c_function(int32_type(), { int32_type() }));
}

TEST_CASE("a type parameter in a C function-pointer signature is legal on a template", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function apply<T>(extern function<T(T)> $f, T $v) : T { return $f($v); }\n"
        "function add1(int32 $x) : int32 { return $x + 1; }\n"
        "echo apply<int32>(&add1, 41);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a private static is not addressable from outside its type", "[cfn]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    private static function origin() : int32 { return 0; }\n"
        "}\n"
        "extern function<int32()> $f = &Point::origin;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "private"));
}
