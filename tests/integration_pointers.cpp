#include <catch2/catch_test_macros.hpp>

#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>
#include <set>
#include <string>

#include "helpers.h"

// whole-pipeline pointer behaviour, observed on the resolved tree
//
// this is the layer between the unit suites (which pin one pass at a time) and tests_eco/ (which
// pins observable output). what lives here is everything that is decided by the passes agreeing
// with each other and is invisible in a program's output: what type an inferred declaration ends
// up with, how many instances a generic produced, what reached a mangled name

using namespace AST;

namespace
{
    ValueType decl_type(Bundle &bundle, const std::string &varname)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *decl : module.nodes.of_type<VarDeclNode>()) {
            if (decl->name_full() == varname && decl->has_type()) {
                return decl->type();
            }
        }
        return ValueType::make_unknown();
    }

    // the concrete (non-template) instances of a generic function, by mangled name
    std::set<std::string> instances_of(Bundle &bundle, const std::string &name)
    {
        std::set<std::string> out;
        auto &module = bundle.modules.find_module("test");
        for (auto *fn : module.nodes.of_type<FunctionDeclNode>()) {
            if (fn->is_anonymous() || fn->is_generic() || fn->func_name() != name) {
                continue;
            }
            out.insert(fn->decorated_func_name());
        }
        return out;
    }
}

TEST_CASE("An inferred declaration keeps an address but reads a value", "[Integration][pointer]")
{
    // two inferences that look alike and are not. `&$a` is not a place - it is already the value
    // it means - so the pointer survives; `$r` is a place holding a pointer, so reading it
    // auto-derefs and the copy is a plain int32 (AST::value_result_type)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$a = 324;\n"
        "$b = &$a;\n"
        "$copy = $b;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    ValueType borrow = decl_type(*bundle, "$b");
    REQUIRE(borrow.is_pointer());
    REQUIRE_FALSE(borrow.is_nullable());
    REQUIRE(borrow.pointee().is_primitive_of_type(ValueTypePrimitive::t_int32));

    REQUIRE(decl_type(*bundle, "$copy").is_primitive_of_type(ValueTypePrimitive::t_int32));
}

TEST_CASE("An inferred declaration nests when the source already holds a pointer", "[Integration][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$a = 1;\n"
        "ptr<int> $p = &$a;\n"
        "$pp = &$p;\n"
        "$addr = $p:$;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // `&$p` is the address of $p's own slot, with no peeling
    REQUIRE(decl_type(*bundle, "$pp").get_type_desciption() == "ptr<int32>&");

    // `$p:$` is the pointer $p holds, so one level shallower
    REQUIRE(decl_type(*bundle, "$addr").get_type_desciption() == "ptr<int32>");
}

TEST_CASE("Generic decay collapses a pointer argument onto the pointee's instance", "[Integration][pointer][generics]")
{
    // the reason the decay rule exists: without it `box($p)` and `box($var)` would produce two
    // instances that behave identically (book/concept/pointers_and_refs_v2.md, "Pointers and generics")
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function box<T>(T $v) : T { return $v; }\n"
        "$var = 7;\n"
        "int& $p = &$var;\n"
        "echo box($p);\n"
        "echo box($var);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    REQUIRE(instances_of(*bundle, "box").size() == 1);
}

TEST_CASE("An explicit ptr<T> parameter instantiates apart from the value form", "[Integration][pointer][generics]")
{
    // asking for a pointer explicitly opts out of the decay, so the two are genuinely different
    // instances - and the mangled names have to differ or they collide in the symbol table
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function box<T>(T $v) : T { return $v; }\n"
        "function hold<T>(ptr<T> $v) : T { return $v; }\n"
        "$var = 7;\n"
        "ptr<int> $p = &$var;\n"
        "echo box($var);\n"
        "echo hold($p);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto boxed = instances_of(*bundle, "box");
    auto held = instances_of(*bundle, "hold");

    REQUIRE(boxed.size() == 1);
    REQUIRE(held.size() == 1);

    // both bound T = int32, but the parameter types differ, so the symbols must too
    REQUIRE(*boxed.begin() != *held.begin());
}

TEST_CASE("Pointer depth reaches the mangled name", "[Integration][pointer][generics]")
{
    // get_mangled_name() encodes each level (R for a level, N nullable / B borrow), so a
    // signature taking ptr<ptr<T>> cannot collide with one taking ptr<T>
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function hold<T>(ptr<T> $v) : void { }\n"
        "$a = 1;\n"
        "ptr<int> $p = &$a;\n"
        "ptr<ptr<int>> $pp = &$p;\n"
        "hold($p);\n"
        "hold($pp);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // ptr<T> against ptr<int32> binds T=int32; against ptr<ptr<int32>> it binds T=ptr<int32>
    REQUIRE(instances_of(*bundle, "hold").size() == 2);
}

TEST_CASE("A borrow parameter infers its type parameter from a value argument", "[Integration][pointer][generics]")
{
    // the mirror of the decay rule: the implicit address-of is inserted against the parameter type,
    // so at inference time `bump($a)` still reads as an int32 against a `T&`. binding through the
    // borrow is what the address-of then produces, so both calls land on the same instance rather
    // than failing to infer T at all
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function bump<T>(T &$v) : void { $v = $v + 1; }\n"
        "$a = 1;\n"
        "$b = 2;\n"
        "bump($a);\n"
        "bump($b);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    REQUIRE(instances_of(*bundle, "bump").size() == 1);
}

TEST_CASE("A nullable pointer parameter does not infer from a value argument", "[Integration][pointer][generics]")
{
    // `ptr<T>` does not auto-borrow, so there is no implicit address-of to anticipate and nothing
    // for T to bind to - the inverse of the case above, and the reason that arm tests nullability
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function hold<T>(ptr<T> $v) : void { }\n"
        "$a = 1;\n"
        "hold($a);\n");

    REQUIRE(EchoTests::has_issue_containing(*bundle, "Cannot infer type parameter 'T'"));
    REQUIRE(instances_of(*bundle, "hold").empty());
}

TEST_CASE("A borrow property resolves on the struct's type", "[Integration][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Holder {\n"
        "    int& $target;\n"
        "    int $count;\n"
        "}\n"
        "$v = 7;\n"
        "$h = Holder(&$v, 1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    ValueType holder = decl_type(*bundle, "$h");
    REQUIRE(holder.is_struct());

    ComplexType *complex = holder.get_complex_type();
    REQUIRE(complex != nullptr);

    ValueType target = complex->get_property_type("target");
    REQUIRE(target.is_pointer());
    REQUIRE_FALSE(target.is_nullable());

    // the neighbouring value property is untouched, so the pointer did not leak across the layout
    REQUIRE(complex->get_property_type("count").is_primitive_of_type(ValueTypePrimitive::t_int32));
}

TEST_CASE("A generic instantiated on a borrow keeps the pointer in its layout", "[Integration][pointer][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $item;\n"
        "}\n"
        "ptr<Box<int&>> $b;\n"
        "ptr<Box<int>> $v;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    ValueType of_borrow = decl_type(*bundle, "$b").pointee();
    ValueType of_value = decl_type(*bundle, "$v").pointee();

    REQUIRE(of_borrow.get_complex_type()->get_property_type("item").is_pointer());
    REQUIRE_FALSE(of_value.get_complex_type()->get_property_type("item").is_pointer());

    // distinct layouts, so the registry must have interned them apart
    REQUIRE(of_borrow.get_complex_type() != of_value.get_complex_type());
}
