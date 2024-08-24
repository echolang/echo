#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTBundle.h>
#include <AST/ASTIssue.h>

#include <string>

// the semantic/type-check pass runs inside tests_make_parsed_bundle (after monomorphization),
// so these assert the diagnostics it records on the collector. errors that previously surfaced
// as context-free codegen throws (or silent voids) now become located, gated issues here.

using namespace AST;

namespace {
    // true if any recorded issue's message contains the given substring.
    bool has_issue_containing(Bundle &bundle, const std::string &needle) {
        for (const auto &issue : bundle.collector.issues) {
            if (issue->message().find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("unknown struct member is a located diagnostic, not a silent void", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $p = point(1, 2);\n"
        "echo $p->z;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "has no member named 'z'"));
    // the diagnostic names the struct it was resolved against
    REQUIRE(has_issue_containing(*bundle, "'point'"));
}

TEST_CASE("valid struct member access produces no critical issues", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $p = point(1, 2);\n"
        "echo $p->x;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("wrong-type argument is a located diagnostic", "[sema]")
{
    // passing a struct where an int parameter is expected: the implicit cast the parser inserts
    // is not a legal conversion, and is caught here instead of crashing codegen with
    // "Unsupported type cast".
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "function takes_int(int $n): int { return $n; }\n"
        "point $p = point(1, 2);\n"
        "takes_int($p);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot implicitly convert 'point' to 'int32'"));
}

TEST_CASE("correct-type arguments produce no critical issues", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add(int $a, int $b): int { return $a + $b; }\n"
        "echo add(2, 3);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("numeric argument conversions are allowed", "[sema]")
{
    // a float argument to an int parameter is a legal implicit numeric conversion and must not
    // be flagged as a type error.
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function takes_int(int $n): int { return $n; }\n"
        "echo takes_int(3);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("use of an unresolvable generic is a critical diagnostic", "[sema]")
{
    // the monomorphizer cannot resolve a call with the wrong number of explicit type arguments;
    // the pipeline reports it as a critical issue rather than reaching codegen. (leftover type
    // parameters in already-concrete code are additionally caught by the type-check pass.)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $x): T { return $x; }\n"
        "echo id<int, int>(5);\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

TEST_CASE("a valid generic instantiation type-checks cleanly", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function twice<T>(T $x): T { return $x + $x; }\n"
        "echo twice(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a binary operator on struct operands is a located diagnostic", "[sema]")
{
    // codegen supports no operator on struct operands; it would otherwise surface as a context-free
    // "unsupported binary operator" deep in codegen. the type-check pass catches it up-front, located
    // at the operator.
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $a = point(1, 2);\n"
        "point $b = point(3, 4);\n"
        "echo $a + $b;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "operator '+'"));
    // the diagnostic names the operand type it was rejected on
    REQUIRE(has_issue_containing(*bundle, "'point'"));
}

TEST_CASE("numeric binary operators are not flagged", "[sema]")
{
    // int + int and a float/int mix are legal for codegen; the struct-operand check must not fire.
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "echo 1 + 2;\n"
        "echo 1.5 * 2;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("per-branch operator gaps are left to the codegen safety net", "[sema]")
{
    // the sema check is intentionally scoped to struct operands; a primitive operator/operand gap
    // (modulo on two bools) is deliberately NOT flagged here and instead surfaces at the enriched
    // codegen throw. this pins that boundary so the scope is not silently widened.
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "echo true % false;\n");

    REQUIRE_FALSE(has_issue_containing(*bundle, "operator '%'"));
}
