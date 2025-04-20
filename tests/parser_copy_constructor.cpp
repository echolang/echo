#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTCopy.h>
#include <AST/ASTMemberLookup.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

namespace
{
    // an owning type with a copy constructor. bodies free of library calls, as everywhere in the unit
    // tests: the harness parses one file with no stdlib module, so a `mem::` call would be an unknown
    // function and drown every assertion. what the real copy *does* is covered end to end in
    // tests_eco/structs/copy_constructor*.eco
    const char *k_box =
        "struct Box {\n"
        "    ptr<uint8> $data;\n"
        "    constructor(ptr<uint8> $d) { $this->data:$ = $d; }\n"
        "    constructor(Box& $other) { $this->data:$ = $other->data; }\n"
        "    destructor() { $this->data:$ = null; }\n"
        "}\n";

    ComplexType &layout_of(Module &m, const std::string &name)
    {
        auto *decl = type_named(m, name);
        REQUIRE(decl != nullptr);
        return decl->complex_type();
    }
}

TEST_CASE("a copy constructor is recognised and published on its type", "[copy_constructor]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_box);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *decl = type_named(m, "Box");
    REQUIRE(decl != nullptr);

    auto *copy_ctor = find_copy_constructor(&decl->complex_type());
    REQUIRE(copy_ctor != nullptr);

    // it is a constructor and nothing else: no MemberKind of its own, because it *is* one
    REQUIRE(copy_ctor->is_constructor());
    REQUIRE(copy_ctor->body != nullptr);
    REQUIRE(copy_ctor->args.size() == 1);
    REQUIRE(copy_ctor->parameter_type(0) == ValueType::make_pointer(decl->value_type(), false));

    // and the copying question keys on it, owning type or not
    REQUIRE(copy_needs_constructor(decl->value_type()));
    REQUIRE(copy_constructor_for(decl->value_type()) == copy_ctor);
}

TEST_CASE("a copy constructor stays in the overload set", "[copy_constructor]")
{
    // the one thing that makes it unlike a destructor, and the whole reason there is no
    // register_copy_constructor: `Box($a)` is a call anyone may write and already resolves, so the
    // explicit copy and the implicit one have to be the very same declaration
    auto bundle = EchoTests::tests_make_parsed_bundle(k_box);

    auto &m = bundle->modules.find_module("test");
    auto &box = layout_of(m, "Box");
    auto *copy_ctor = find_copy_constructor(&box);

    auto candidates = bundle->collector.functions.overloads("Box", *box.ast_namespace);

    bool found = false;
    for (auto *candidate : candidates) {
        found = found || candidate == copy_ctor;
    }

    REQUIRE(found);

    // and it is not in the method table either way - a constructor never was
    REQUIRE(find_member_functions(&box, "Box").empty());
}

TEST_CASE("a const borrow of the own type is a copy constructor too", "[copy_constructor]")
{
    // the more honest spelling, since a copy reads its source. const is dropped on both sides of the
    // comparison, and the plain `Box&` receiver the ownership pass builds still fits it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box {\n"
        "    ptr<uint8> $data;\n"
        "    constructor(ptr<uint8> $d) { $this->data:$ = $d; }\n"
        "    constructor(const Box& $other) { $this->data:$ = $other->data; }\n"
        "    destructor() { $this->data:$ = null; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(find_copy_constructor(&layout_of(m, "Box")) != nullptr);
}

TEST_CASE("what is not a copy constructor", "[copy_constructor]")
{
    SECTION("a nullable pointer to the own type") {
        // `ptr<Box>` takes an address that may be nothing, which is a legitimately different
        // constructor - and one whose parameter cannot stand in for a value
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Box {\n"
            "    ptr<uint8> $data;\n"
            "    constructor(ptr<Box> $other) { $this->data:$ = null; }\n"
            "    destructor() { $this->data:$ = null; }\n"
            "}\n");

        auto &m = bundle->modules.find_module("test");
        REQUIRE(find_copy_constructor(&layout_of(m, "Box")) == nullptr);
    }

    SECTION("a borrow of the own type plus a second parameter") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Box {\n"
            "    ptr<uint8> $data;\n"
            "    constructor(Box& $other, int32 $extra) { $this->data:$ = $other->data; }\n"
            "    destructor() { $this->data:$ = null; }\n"
            "}\n");

        auto &m = bundle->modules.find_module("test");
        REQUIRE(find_copy_constructor(&layout_of(m, "Box")) == nullptr);
    }

    SECTION("the synthesized field-wise constructor of a struct holding a borrow of itself") {
        // the trap worth pinning: `Odd`'s field-wise constructor takes exactly one `Odd&` and so is
        // signature-identical to a copy constructor. it never goes through parse_constructor, which is
        // what keeps it from being mistaken for one - no flag needed
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Odd {\n"
            "    Odd& $other;\n"
            "}\n");

        auto &m = bundle->modules.find_module("test");
        REQUIRE(find_copy_constructor(&layout_of(m, "Odd")) == nullptr);
    }
}

TEST_CASE("a generic's instantiation finds its copy constructor through its template", "[copy_constructor]")
{
    // `Box<int32>` holds no copy constructor of its own - the same template_ref redirect find_destructor
    // does. that redirect is what lets TypeRegistry know nothing about members
    // the first constructor deliberately takes a `usize` rather than a `ptr<T>`: `null` would fit both
    // that and the copy constructor's borrow, and the call would be an honest ambiguity
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    ptr<T> $slot;\n"
        "    constructor(usize $t) { $this->slot:$ = null; }\n"
        "    constructor(Box<T>& $other) { $this->slot:$ = $other->slot; }\n"
        "    destructor() { $this->slot:$ = null; }\n"
        "}\n"
        "function f() : void {\n"
        "    $a = Box<int32>(1);\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto &tmpl = layout_of(m, "Box");

    auto *template_copy = find_copy_constructor(&tmpl);
    REQUIRE(template_copy != nullptr);

    // the instantiation's own slot is empty, and the lookup redirects to the template's declaration
    ComplexType *instance = bundle->collector.type_registry.get_or_create_instantiation(
        &tmpl, {ValueType(ValueTypePrimitive::t_int32)});

    REQUIRE(instance != &tmpl);
    REQUIRE(instance->copy_constructor() == nullptr);
    REQUIRE(find_copy_constructor(instance) == template_copy);
}

TEST_CASE("a type may declare only one copy constructor", "[copy_constructor]")
{
    SECTION("two identically shaped ones are a duplicate signature") {
        // already reported by the registration, at the second one's own `constructor` keyword, so
        // recognition adds nothing here
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Box {\n"
            "    ptr<uint8> $data;\n"
            "    constructor(Box& $other) { $this->data:$ = $other->data; }\n"
            "    constructor(Box& $another) { $this->data:$ = $another->data; }\n"
            "    destructor() { $this->data:$ = null; }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "is already declared with these parameter types"));
    }

    SECTION("a borrow beside a const borrow needs its own diagnostic") {
        // two different signatures, so the registration sees no duplicate - but both are the copy, and
        // nothing would decide which of them an implicit copy meant
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Box {\n"
            "    ptr<uint8> $data;\n"
            "    constructor(Box& $other) { $this->data:$ = $other->data; }\n"
            "    constructor(const Box& $another) { $this->data:$ = $another->data; }\n"
            "    destructor() { $this->data:$ = null; }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "already has a copy constructor"));
    }
}

TEST_CASE("a bare template name is not the borrow of a generic's self type", "[copy_constructor]")
{
    // inside `struct Box<T>`, `Box` resolves to the template, which is not a type any value has. said
    // out loud rather than left silent, because the alternative is a rejected copy somewhere else
    // entirely, naming a type whose author believes they wrote a copy constructor for it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    ptr<T> $slot;\n"
        "    constructor(Box& $other) { $this->slot:$ = $other->slot; }\n"
        "    destructor() { $this->slot:$ = null; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "names the template rather than a type"));
}
