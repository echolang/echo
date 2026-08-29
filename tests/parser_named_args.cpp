#include <catch2/catch_test_macros.hpp>

#include <AST/FunctionDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;

TEST_CASE("a parameter label is recorded on the declaration", "[named_args]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function listen(forEvent: string $name, int32 $code) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto decls = decls_named(bundle->modules.find_module("test"), "listen");
    REQUIRE(decls.size() == 1);

    auto *decl = decls[0];
    REQUIRE(decl->args.size() == 2);
    REQUIRE(decl->args[0]->has_label());
    REQUIRE(decl->args[0]->label() == "forEvent");
    REQUIRE(decl->args[0]->name() == "name");
    REQUIRE_FALSE(decl->args[1]->has_label());
    REQUIRE(decl->args[1]->name() == "code");
}

TEST_CASE("two labelled overloads of the same types are distinct", "[named_args]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function print(int32 $level, string $message) : void {}\n"
        "function print(fromDecimal: int32 $cents, currency: string $code) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto decls = decls_named(bundle->modules.find_module("test"), "print");
    REQUIRE(decls.size() == 2);
}

TEST_CASE("two unlabelled overloads of the same types still collide", "[named_args]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : int32 { return 1; }\n"
        "function f(int32 $b) : int32 { return 2; }\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

TEST_CASE("two parameters cannot share a label", "[named_args]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(from: int32 $a, from: string $b) : void {}\n");

    REQUIRE(has_issue_containing(*bundle, "The label 'from' is already used"));
}
