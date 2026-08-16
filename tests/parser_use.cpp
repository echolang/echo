#include <catch2/catch_test_macros.hpp>

#include "AST/ASTImport.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ASTValueType.h"
#include "AST/TypeDeclNode.h"

#include "helpers.h"

TEST_CASE("use binds a namespace prefix for the whole file", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace geometry;

        struct Point { int32 $x; int32 $y; }

        function make(int32 $x, int32 $y) : Point
        {
            return Point($x, $y);
        }

        namespace app;

        use geometry;

        function run() : int32
        {
            geometry::Point $p = geometry::make(3, 4);
            return $p->x;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *file = bundle->modules.find_module("test").files().first();
    REQUIRE(file != nullptr);
    REQUIRE(file->imports.size() == 1);
    REQUIRE(file->imports[0].kind == AST::ImportKind::t_namespace);
    REQUIRE(file->imports[0].local_name == "geometry");
}

TEST_CASE("use of a type names the type and its constructor", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace geometry;

        struct Point { int32 $x; int32 $y; }

        namespace app;

        use geometry::Point;

        function run() : int32
        {
            Point $p = Point(3, 4);
            return $p->x + $p->y;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *file = bundle->modules.find_module("test").files().first();
    REQUIRE(file != nullptr);
    REQUIRE(file->imports.size() == 1);
    REQUIRE(file->imports[0].kind == AST::ImportKind::t_item);
    REQUIRE(file->imports[0].local_name == "Point");
    REQUIRE(file->imports[0].target_name == "Point");

    auto calls = EchoTests::calls_to(bundle->modules.find_module("test"), "Point");
    REQUIRE_FALSE(calls.empty());
    REQUIRE(calls[0]->lookup_namespace != nullptr);
    REQUIRE(calls[0]->lookup_namespace->full_name() == "geometry");
}

TEST_CASE("an aliased use looks the function up under its real name", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace geometry;

        function make(int32 $x) : int32
        {
            return $x;
        }

        namespace app;

        use geometry::make as build;

        function run() : int32
        {
            return build(7);
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto calls = EchoTests::calls_to(bundle->modules.find_module("test"), "build");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->lookup_name() == "make");
    REQUIRE(calls[0]->imported_name == "make");
    REQUIRE(calls[0]->lookup_namespace != nullptr);
    REQUIRE(calls[0]->lookup_namespace->full_name() == "geometry");
}

TEST_CASE("a grouped use does not swallow the next type in pass 1", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace geometry;

        struct Point { int32 $x; }

        function make(int32 $x) : Point
        {
            return Point($x);
        }

        namespace app;

        use geometry::{
            Point,
            make,
        };

        struct Box { Point $p; }

        function run() : int32
        {
            return make(1)->x;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *box = EchoTests::type_named(bundle->modules.find_module("test"), "Box");
    REQUIRE(box != nullptr);
}

TEST_CASE("a use is file-local", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        R"(
            namespace geometry;

            function make(int32 $x) : int32 { return $x; }

            namespace app;

            use geometry::make;

            function from_here() : int32 { return make(1); }
        )",
        R"(
            namespace app;

            function from_there() : int32 { return make(2); }
        )"
    });

    REQUIRE(EchoTests::count_issues_containing(*bundle, "could not be found") >= 1);
}

TEST_CASE("a duplicate use of the same local name is refused", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace a;
        function f() : int32 { return 1; }

        namespace b;
        function f() : int32 { return 2; }

        namespace app;
        use a::f;
        use b::f;
    )");

    REQUIRE(EchoTests::count_issues_containing(*bundle, "already imported") == 1);
}

TEST_CASE("use inside a body is refused", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        function run() : void
        {
            use std::math;
        }
    )");

    REQUIRE(EchoTests::count_issues_containing(*bundle, "cannot appear inside a body") == 1);
}

TEST_CASE("an unknown use path is refused", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        use no::such::path;
    )");

    REQUIRE(EchoTests::count_issues_containing(*bundle, "Unknown namespace") == 1);
}

TEST_CASE("a same-module imported constant is visible to a later constant", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace a;
        const MAX = 5;

        namespace b;
        use a::MAX;
        const LIMIT = MAX;

        function run() : int32
        {
            return LIMIT;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a prefix use reaches a static method on a type in that namespace", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace geometry;

        struct Point
        {
            int32 $x;

            static function origin() : Point
            {
                return Point(0);
            }
        }

        namespace app;

        use geometry;

        function run() : int32
        {
            geometry::Point $p = geometry::Point::origin();
            return $p->x;
        }
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto calls = EchoTests::calls_to(bundle->modules.find_module("test"), "origin");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->static_owner.has_complex_type());
    REQUIRE(calls[0]->static_owner.get_type_desciption().find("Point") != std::string::npos);
}

TEST_CASE("a same-module imported constant is visible when declared after the use", "[use]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        namespace b;
        use a::MAX;
        const LIMIT = MAX;

        function run() : int32
        {
            return LIMIT;
        }

        namespace a;
        const MAX = 5;
    )");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}
