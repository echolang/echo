#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAtomicity.h>
#include <AST/ASTBundle.h>
#include <AST/ExprNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

TEST_CASE("#[atomic] marks a class and nothing else", "[atomicity]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[atomic]\n"
        "class Marked { int32 $n; }\n"
        "class Plain { int32 $n; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(counts_are_atomic(type_named(m, "Marked")->complex_type()));
    REQUIRE_FALSE(counts_are_atomic(type_named(m, "Plain")->complex_type()));
}

TEST_CASE("#[atomic] is carried onto an instantiation", "[atomicity]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[atomic]\n"
        "class Box<T> { T $v; }\n"
        "function take(Box<int32> $b) : int32 { return 0; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    ComplexType &tmpl = type_named(m, "Box")->complex_type();
    REQUIRE(tmpl.is_atomic);

    ComplexType *of_int = bundle->collector.type_registry.get_or_create_instantiation(
        &tmpl, { ValueType(ValueTypePrimitive::t_int32) });
    REQUIRE(of_int != nullptr);
    REQUIRE(counts_are_atomic(*of_int));
}

TEST_CASE("#[atomic] survives a use before the class is declared", "[atomicity]")
{
    // the signature interns Box<int32> during the declaration pass, before
    // bind_atomic_attribute runs. a property-less class never looks stale
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function take(Box<int32> $b) : int32 { return 0; }\n"
        "#[atomic]\n"
        "class Box<T> {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    ComplexType &tmpl = type_named(m, "Box")->complex_type();
    REQUIRE(tmpl.is_atomic);

    ComplexType *of_int = bundle->collector.type_registry.get_or_create_instantiation(
        &tmpl, { ValueType(ValueTypePrimitive::t_int32) });
    REQUIRE(of_int != nullptr);
    REQUIRE(counts_are_atomic(*of_int));
}

TEST_CASE("a closure environment counts atomically", "[atomicity]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function main() : int32 {\n"
        "    int32 $n = 1;\n"
        "    function<int32()> $f = function() : int32 { return $n; };\n"
        "    return $f();\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    bool saw = false;

    for (ClosureExprNode *closure : m.nodes.of_type<ClosureExprNode>()) {
        REQUIRE(closure->environment_type != nullptr);
        REQUIRE(counts_are_atomic(*closure->environment_type));
        saw = true;
    }

    REQUIRE(saw);
}

TEST_CASE("#[atomic] is refused on a struct and an interface", "[atomicity]")
{
    auto refused = EchoTests::tests_make_parsed_bundle(
        "#[atomic]\n"
        "struct Point { int32 $x; }\n"
        "#[atomic]\n"
        "interface Drawable { function draw() : void; }\n");

    REQUIRE(has_issue_containing(*refused, "cannot be written on a struct"));
    REQUIRE(has_issue_containing(*refused, "cannot be written on an interface"));

    auto enum_refused = EchoTests::tests_make_parsed_bundle(
        "#[atomic]\n"
        "enum Color { case red; }\n");

    REQUIRE(has_issue_containing(*enum_refused, "cannot be written on an enum"));
}
