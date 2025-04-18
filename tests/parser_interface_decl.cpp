#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConformance.h>
#include <AST/ASTMemberLookup.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::count_issues_containing;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

namespace
{
    const char *k_drawable =
        "interface Drawable {\n"
        "    function draw() : void;\n"
        "    function area() : float64;\n"
        "}\n"
        "struct Square : Drawable {\n"
        "    float64 $side;\n"
        "    function draw() : void { echo 1; }\n"
        "    function area() : float64 { return $this->side; }\n"
        "}\n";
}

// an interface is a third ComplexTypeKind on the same TypeDeclNode, so what this pins is that the one
// bit reached the layout - everything else about the declaration (namespace, mangling, member store) is
// shared code that a wrong kind would silently take the struct path through
TEST_CASE("an interface is a declared type of its own kind", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_drawable);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *drawable = type_named(m, "Drawable");
    REQUIRE(drawable != nullptr);

    REQUIRE(drawable->kind() == ComplexTypeKind::t_interface);
    REQUIRE(drawable->complex_type().is_interface_kind());
    REQUIRE(drawable->value_type().is_interface());

    // a named user type for every question that means "can it have members", and not one for any
    // question that means "does it have storage". those were one predicate while there were two kinds
    REQUIRE(drawable->value_type().has_complex_type());
    REQUIRE_FALSE(drawable->value_type().has_property_layout());
    REQUIRE(drawable->complex_type().property_count() == 0);
}

// the requirements are ordinary member declarations in the ordinary member store, which is what makes
// find_member_functions - and therefore a member call through an interface receiver - work with no arm
// of its own. the *order* is the assertion that matters beyond that: it is vtable slot order
TEST_CASE("interface requirements are methods on the type, in declaration order", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_drawable);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *drawable = type_named(m, "Drawable");
    REQUIRE(drawable != nullptr);

    const auto requirements = AST::interface_requirements(&drawable->complex_type());
    REQUIRE(requirements.size() == 2);
    REQUIRE(requirements[0]->func_name() == "draw");
    REQUIRE(requirements[1]->func_name() == "area");

    // the slot is the ordinal in that same list, answered by one function so a vtable and a dispatch
    // site can never disagree about which entry a method is
    REQUIRE(AST::interface_method_slot(&drawable->complex_type(), requirements[0]) == 0);
    REQUIRE(AST::interface_method_slot(&drawable->complex_type(), requirements[1]) == 1);

    // a method of a *different* type is in no slot at all
    auto *square = type_named(m, "Square");
    REQUIRE(square != nullptr);
    const auto own = AST::find_member_functions(&square->complex_type(), "draw");
    REQUIRE(own.size() == 1);
    REQUIRE_FALSE(AST::interface_method_slot(&drawable->complex_type(), own[0]).has_value());
}

// a requirement has no symbol - the implementors have the bodies - and this is the predicate the two
// build_function_maps loops and gen_function_decl skip on. getting it wrong emits a `declare` nobody
// defines, which fails at link time rather than at the mistake
TEST_CASE("an interface requirement is not a function anything emits", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_drawable);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *drawable = type_named(m, "Drawable");
    auto *square = type_named(m, "Square");
    REQUIRE(drawable != nullptr);
    REQUIRE(square != nullptr);

    for (auto *requirement : AST::interface_requirements(&drawable->complex_type())) {
        REQUIRE(requirement->is_interface_requirement());
        REQUIRE(requirement->body == nullptr);
    }

    // ...and an implementor's method of the same name is emphatically not one
    for (auto *method : square->complex_type().methods()) {
        REQUIRE_FALSE(method->is_interface_requirement());
    }
}

// the conformance clause publishes onto the type, and only what is valid: the refusals below each
// decline to *publish*, so no reader ever has to re-filter this list
TEST_CASE("a conformance clause publishes the interface on the implementor", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_drawable);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *drawable = type_named(m, "Drawable");
    auto *square = type_named(m, "Square");
    REQUIRE(drawable != nullptr);
    REQUIRE(square != nullptr);

    // published once, though both later parse passes walk the clause
    const auto &published = square->complex_type().conformances();
    REQUIRE(published.size() == 1);
    REQUIRE(published[0] == drawable->value_type());

    REQUIRE(AST::conforms_to(&square->complex_type(), drawable->value_type()));
    REQUIRE(AST::conforms_to(square->value_type(), drawable->value_type()));

    // and the interface does not conform to itself, which would make the runtime table circular
    REQUIRE_FALSE(AST::conforms_to(&drawable->complex_type(), drawable->value_type()));
}

TEST_CASE("a conformance clause takes more than one interface", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface A { function a() : void; }\n"
        "interface B { function b() : void; }\n"
        "struct S : A, B {\n"
        "    int32 $x;\n"
        "    function a() : void { echo 1; }\n"
        "    function b() : void { echo 2; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *s = type_named(m, "S");
    REQUIRE(s != nullptr);
    REQUIRE(s->complex_type().conformances().size() == 2);

    REQUIRE(AST::conforms_to(&s->complex_type(), type_named(m, "A")->value_type()));
    REQUIRE(AST::conforms_to(&s->complex_type(), type_named(m, "B")->value_type()));
}

// a class conforms exactly as a struct does. the difference between them shows up at runtime and in
// what an interface *value* may hold - not in whether the conformance is declared or checked
TEST_CASE("a class conforms the same way a struct does", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Drawable { function draw() : void; }\n"
        "class Circle : Drawable {\n"
        "    float64 $r;\n"
        "    function draw() : void { echo 1; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *circle = type_named(m, "Circle");
    REQUIRE(circle != nullptr);
    REQUIRE(circle->kind() == ComplexTypeKind::t_class);
    REQUIRE(AST::conforms_to(&circle->complex_type(), type_named(m, "Drawable")->value_type()));
}

// every shape an interface body cannot hold. one section each rather than one program with seven
// mistakes in it, because parser error recovery interacts: a refused member consumes to the end of its
// own scope, so two adjacent refusals in one body report the first and swallow the second
TEST_CASE("what an interface body cannot declare", "[interface]")
{
    SECTION("a property") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { int32 $count; }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot declare a property"));
    }

    SECTION("a method with a body") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f() : void { echo 1; } }\n");
        REQUIRE(has_issue_containing(*bundle, "so it cannot have a body"));
    }

    SECTION("a constructor") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { constructor() {} }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot declare a constructor"));
    }

    SECTION("a destructor") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { destructor {} }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot declare a destructor"));
    }

    SECTION("a nested type") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { struct Inner { int32 $x; } }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot declare a nested type"));
    }
}

TEST_CASE("what a conformance clause cannot name", "[interface]")
{
    SECTION("a struct is not an interface") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct A { int32 $x; }\n"
            "struct B : A { int32 $y; }\n");
        REQUIRE(has_issue_containing(*bundle, "is not an interface"));
    }

    SECTION("a class is not an interface either") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "class A { int32 $x; }\n"
            "struct B : A { int32 $y; }\n");
        REQUIRE(has_issue_containing(*bundle, "is not an interface"));
    }

    SECTION("the same interface twice") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f() : void; }\n"
            "struct S : I, I { int32 $x; function f() : void { echo 1; } }\n");
        REQUIRE(has_issue_containing(*bundle, "already conforms to"));
    }

    SECTION("an interface conforming to an interface") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f() : void; }\n"
            "interface J : I { function g() : void; }\n");
        REQUIRE(has_issue_containing(*bundle, "an interface cannot conform to another one"));
    }
}

// `#[implicit]` returning an interface would be a *second* route to the same conversion, ranked
// differently from the widening the compiler already knows how to do - and which one fired would
// depend on nothing the author wrote
TEST_CASE("an implicit conversion cannot target an interface", "[interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Drawable { function draw() : void; }\n"
        "struct Square : Drawable {\n"
        "    float64 $side;\n"
        "    function draw() : void { echo 1; }\n"
        "    #[implicit]\n"
        "    function as_drawable() : Drawable { return $this->side; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "cannot return the interface"));
}

// an interface may be declared anywhere and used everywhere, because the type-name pass publishes every
// declaration in every file before the declaration pass reads a single conformance clause
TEST_CASE("an interface is usable above its own declaration and across files", "[interface]")
{
    SECTION("above it in the same file") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Square : Drawable {\n"
            "    float64 $side;\n"
            "    function draw() : void { echo 1; }\n"
            "}\n"
            "interface Drawable { function draw() : void; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        REQUIRE(AST::conforms_to(
            &type_named(m, "Square")->complex_type(), type_named(m, "Drawable")->value_type()));
    }

    SECTION("declared in another file") {
        auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string> {
            "struct Square : Drawable {\n"
            "    float64 $side;\n"
            "    function draw() : void { echo 1; }\n"
            "}\n",
            "interface Drawable { function draw() : void; }\n",
        });

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        REQUIRE(AST::conforms_to(
            &type_named(m, "Square")->complex_type(), type_named(m, "Drawable")->value_type()));
    }
}
