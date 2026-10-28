#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTSymbol.h>
#include <AST/TypeAliasDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/TypeNode.h>
#include <Parser/TypeAliasParser.h>

#include <string>
#include <vector>

#include "helpers.h"

using EchoTests::has_issue_containing;
using EchoTests::prim;
using EchoTests::type_named;

namespace
{
    bool starts_alias(const std::string &content)
    {
        auto env = EchoTests::tests_make_parser_env(content);
        const auto before = env.payload.cursor.snapshot();

        const bool answer = Parser::starts_type_alias(env.payload.cursor);

        REQUIRE(env.payload.cursor.snapshot().index == before.index);

        return answer;
    }

    AST::TypeAliasDeclNode *alias_named(AST::Bundle &bundle, const std::string &name)
    {
        AST::Symbol *symbol = bundle.collector.namespaces.find_symbol(name, bundle.collector.namespaces.root());

        if (symbol == nullptr || symbol->type() != AST::SymbolType::t_alias) {
            return nullptr;
        }

        return symbol->node.get_ptr<AST::TypeAliasDeclNode>();
    }
}

// `type` is an identifier, so only the shape after it makes a declaration
TEST_CASE("what starts a type alias", "[type_alias]")
{
    CHECK(starts_alias("type Query = int32;"));
    CHECK(starts_alias("type Page<T> = array<T>;"));

    CHECK_FALSE(starts_alias("type $x = 1;"));
    CHECK_FALSE(starts_alias("type(1);"));
    CHECK_FALSE(starts_alias("type::make();"));
    CHECK_FALSE(starts_alias("type Iter : Stepper<V>;"));
    CHECK_FALSE(starts_alias("type Iter;"));
}

TEST_CASE("an alias is its target", "[type_alias]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $v; }\n"
        "type Ints = Box<int32>;\n"
        "type Page<T> = Box<T>;\n"
        "Ints $a = Ints(1);\n"
        "Page<int32> $b = $a;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::TypeAliasDeclNode *ints = alias_named(*bundle, "Ints");
    AST::TypeAliasDeclNode *page = alias_named(*bundle, "Page");
    REQUIRE(ints != nullptr);
    REQUIRE(page != nullptr);

    REQUIRE(ints->resolution == AST::TypeAliasResolution::t_resolved);
    REQUIRE(ints->target != nullptr);
    REQUIRE(ints->target->type.has_complex_type());
    REQUIRE(ints->target->type.get_complex_type()->instantiation_args
        == std::vector<AST::ValueType>{ prim(AST::ValueTypePrimitive::t_int32) });

    REQUIRE(page->is_generic());
    REQUIRE(page->type_parameters.size() == 1);
    REQUIRE(page->type_parameters[0]->owner_kind() == AST::TypeParamOwnerKind::t_alias);
    REQUIRE(page->type_parameters[0]->describe() == "Page::T");
}

// the step between the first two passes loops until no alias is left that names an unresolved one, so
// neither the file an alias is written in nor the order inside it matters
TEST_CASE("aliases resolve whatever order they are written in", "[type_alias]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "type Outer = Middle;\n"
        "Outer $x = 5;\n",
        "type Middle = Inner;\n"
        "type Inner = int64;\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::TypeAliasDeclNode *outer = alias_named(*bundle, "Outer");
    REQUIRE(outer != nullptr);
    REQUIRE(outer->target != nullptr);
    REQUIRE(outer->target->type == prim(AST::ValueTypePrimitive::t_int64));
}

TEST_CASE("what an alias refuses", "[type_alias]")
{
    SECTION("a cycle") {
        auto bundle = EchoTests::tests_make_parsed_bundle("type A = B;\ntype B = A;\n");
        REQUIRE(has_issue_containing(*bundle, "The type alias 'A' names itself"));
        REQUIRE(alias_named(*bundle, "A")->resolution == AST::TypeAliasResolution::t_cyclic);
    }

    SECTION("type arguments on a plain alias") {
        auto bundle = EchoTests::tests_make_parsed_bundle("type Id = int32;\nId<int32> $x = 1;\n");
        REQUIRE(has_issue_containing(*bundle, "so it takes no type arguments"));
    }

    SECTION("a generic alias used bare") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Box<T> { T $v; }\ntype Page<T> = Box<T>;\nPage $p;\n");
        REQUIRE(has_issue_containing(*bundle, "'Page' is generic, so it needs its type arguments"));
    }

    SECTION("the wrong number of type arguments") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Box<T> { T $v; }\ntype Page<T> = Box<T>;\nPage<int32, int32> $p;\n");
        REQUIRE(has_issue_containing(*bundle, "Wrong number of type arguments for generic type 'Page'"));
    }

    SECTION("inside a struct") {
        auto bundle = EchoTests::tests_make_parsed_bundle("struct S { type Inner = int32; int32 $x; }\n");
        REQUIRE(has_issue_containing(*bundle, "A type alias cannot be declared inside 'S'"));
    }

    SECTION("inside a body") {
        auto bundle = EchoTests::tests_make_parsed_bundle("function f() : void { type Inner = int32; }\n");
        REQUIRE(has_issue_containing(*bundle, "A type alias cannot be declared inside a body"));
    }

    SECTION("a name a type already has") {
        auto bundle = EchoTests::tests_make_parsed_bundle("struct Point { int32 $x; }\ntype Point = int64;\n");
        REQUIRE(has_issue_containing(*bundle, "The type 'Point' is already declared"));
    }

    SECTION("a type taking an alias's name") {
        auto bundle = EchoTests::tests_make_parsed_bundle("type Point = int64;\nstruct Point { int32 $x; }\n");
        REQUIRE(has_issue_containing(*bundle, "The type 'Point' is already declared"));
    }
}

TEST_CASE("a nested type through an applied alias", "[type_alias]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct World { struct Moon { int32 $phase; } }\n"
        "type Page<T> = World;\n"
        "Page<int32>::Moon $m = Page<int32>::Moon(7);\n"
        "echo $m->phase;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::TypeDeclNode *moon = type_named(bundle->modules.find_module("test"), "Moon");
    REQUIRE(moon != nullptr);
}
