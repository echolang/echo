#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

using EchoTests::has_issue_containing;

// B21: an inferred declaration whose initializer is a call that fails to parse an argument used
// to abort on ParserCursor::current after recovery consumed the `;`. the stdlib-less harness is
// what surfaced it - `mem::alloc` is unknown here the same way a typo'd namespace is in a real
// program

TEST_CASE("an unknown namespaced generic call as a constructor argument reports", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { ptr<T> $slot; }\n"
        "$b = Box<int32>(nope::alloc<int32>(1));\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "could not be found"));
}

TEST_CASE("an unknown call as a constructor argument of an inferred declaration reports", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { ptr<T> $slot; }\n"
        "$b = Box<int32>(nope());\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "could not be found"));
}

TEST_CASE("a written type on that declaration still reports the unknown function", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { ptr<T> $slot; }\n"
        "Box<int32> $b = Box<int32>(nope::alloc<int32>(1));\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "could not be found"));
}

TEST_CASE("an unknown namespaced generic call as a statement reports", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("nope::alloc<int32>(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "could not be found"));
}

TEST_CASE("an inferred declaration from an unknown free call reports", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("$b = nope();\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "could not be found"));
}

// B38: `&f()` in a declaration initializer used to run the cursor off the end. it is a
// function-ref plus a postfix call now, so the program compiles; the lock is that it does
// not abort
TEST_CASE("address-of a free call in an inferred declaration does not abort", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f() : int32 { return 1; }\n"
        "$r = &f();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}
