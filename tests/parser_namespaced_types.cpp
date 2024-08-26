#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTNamespace.h>
#include <AST/FunctionDeclNode.h>
#include <AST/StructNode.h>

// namespace qualified type names (`a::Foo`) and the mangling that keeps same-named types from
// different namespaces apart. see todo/B7-complex-type-mangling.md

namespace {
    // the struct declared under the given namespaced name, or null when there is none
    AST::StructDeclNode *find_struct(AST::Bundle &bundle, const std::string &namespaced_name)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *strct : module.nodes.of_type<AST::StructDeclNode>()) {
            if (strct->namespaced_struct_name() == namespaced_name) {
                return strct;
            }
        }
        return nullptr;
    }

    AST::FunctionDeclNode *find_function(AST::Bundle &bundle, const std::string &namespaced_name)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *func : module.nodes.of_type<AST::FunctionDeclNode>()) {
            if (func->namespaced_func_name() == namespaced_name) {
                return func;
            }
        }
        return nullptr;
    }
}

TEST_CASE("A qualified type name resolves in the named namespace", "[namespaces][types]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace a;

        struct Point { int $x; int $y; }

        namespace b;

        struct Wrapper { a::Point $point; }

        function distance(a::Point $p) : int
        {
            return $p->x + $p->y;
        }

        function origin() : a::Point
        {
            return a::Point(0, 0);
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *point = find_struct(*bundle, "a::Point");
    REQUIRE(point != nullptr);

    // the property, the argument and the return type all resolved to the very same type
    auto *wrapper = find_struct(*bundle, "b::Wrapper");
    REQUIRE(wrapper != nullptr);
    REQUIRE(wrapper->value_type().get_complex_type()->get_property_type("point") == point->value_type());

    auto *distance = find_function(*bundle, "b::distance");
    REQUIRE(distance != nullptr);
    REQUIRE(distance->args.size() == 1);
    REQUIRE(distance->args[0]->type_node()->type == point->value_type());

    auto *origin = find_function(*bundle, "b::origin");
    REQUIRE(origin != nullptr);
    REQUIRE(origin->get_return_type() == point->value_type());
}

TEST_CASE("A qualified type name can be a generic argument", "[namespaces][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace a;

        struct Point { int $x; }

        namespace b;

        struct Box<T> { T $item; }

        function unwrap(Box<a::Point> $box) : int
        {
            return $box->item->x;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *point = find_struct(*bundle, "a::Point");
    REQUIRE(point != nullptr);

    auto *unwrap = find_function(*bundle, "b::unwrap");
    REQUIRE(unwrap != nullptr);

    auto arg_type = unwrap->args[0]->type_node()->type;
    REQUIRE(arg_type.is_struct());

    auto *box_of_point = arg_type.get_complex_type();
    REQUIRE(box_of_point->is_instantiated());
    REQUIRE(box_of_point->instantiation_args.size() == 1);
    REQUIRE(box_of_point->instantiation_args[0] == point->value_type());
    REQUIRE(box_of_point->get_property_type("item").get_type_desciption() == "a::Point");
}

TEST_CASE("An unresolvable qualified type name is reported", "[namespaces][types]")
{
    EchoTests::assert_code_emits_issue(R"(
        namespace a;

        struct Point { int $x; }

        function distance(a::Vector $v) : int
        {
            return 0;
        }
    )", "Unknown type 'a::Vector'");
}

TEST_CASE("Same-named structs from different namespaces mangle apart", "[namespaces][types]")
{
    // one file per namespace: a same-named struct's constructor would otherwise shadow its
    // twin in the shared file scope, which is a separate limitation of function resolution
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        R"(
            namespace a;

            struct Foo { int $x; }

            function make() : Foo
            {
                return Foo(11);
            }
        )",
        R"(
            namespace b;

            struct Foo { int $x; int $y; }

            function take(a::Foo $f) : int
            {
                return $f->x;
            }

            function take_own(Foo $f) : int
            {
                return $f->y;
            }
        )"
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *foo_a = find_struct(*bundle, "a::Foo");
    auto *foo_b = find_struct(*bundle, "b::Foo");
    REQUIRE(foo_a != nullptr);
    REQUIRE(foo_b != nullptr);

    // the types themselves and the symbols of the functions taking them stay distinct
    REQUIRE(foo_a->value_type().get_mangled_name() != foo_b->value_type().get_mangled_name());

    auto *take = find_function(*bundle, "b::take");
    auto *take_own = find_function(*bundle, "b::take_own");
    REQUIRE(take != nullptr);
    REQUIRE(take_own != nullptr);
    REQUIRE(take->args[0]->type_node()->type == foo_a->value_type());
    REQUIRE(take_own->args[0]->type_node()->type == foo_b->value_type());

    // the decorated names differ only in the argument type, which is exactly the collision
    // the mangled complex-type token has to prevent
    std::string decorated_take = take->decorated_func_name();
    std::string decorated_take_own = take_own->decorated_func_name();
    REQUIRE(decorated_take != decorated_take_own);
}

TEST_CASE("Namespaced names walk the full namespace path", "[namespaces]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace deep::nested;

        struct Foo { int $x; }

        function work(Foo $f) : int
        {
            return $f->x;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *foo = find_struct(*bundle, "deep::nested::Foo");
    REQUIRE(foo != nullptr);

    auto *work = find_function(*bundle, "deep::nested::work");
    REQUIRE(work != nullptr);

    // root first, one separator per segment and no empty segment for the root
    REQUIRE(work->decorated_func_name().starts_with("_deep_nested_workZ"));
}

TEST_CASE("A root namespace name carries no separator", "[namespaces]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        struct Foo { int $x; }

        function work(Foo $f) : int
        {
            return $f->x;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
    REQUIRE(find_struct(*bundle, "Foo") != nullptr);

    auto *work = find_function(*bundle, "work");
    REQUIRE(work != nullptr);
    REQUIRE(work->decorated_func_name().starts_with("_workZ"));
}
