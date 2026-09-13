#include <catch2/catch_test_macros.hpp>

#include <AST/ASTOperatorSemantics.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

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

// a constructor registered in pass 1 whose parameter failed to parse used to abort in the body
// pass: parse_parameter_list keeps a nullptr slot so arity is preserved, and BodylessFunction
// printed the signature by walking it. the lock is that it reports rather than SIGSEGV
TEST_CASE("a bodyless constructor with a failed parameter still reports", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo {\n"
        "    constructor(int32);\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "was declared but never given a body"));

    auto &m = bundle->modules.find_module("test");
    auto *foo = type_named(m, "Foo");
    REQUIRE(foo != nullptr);
    REQUIRE_FALSE(foo->constructors().empty());

    // recovery may leave more than one hole; the lock is that the walk is defined
    const std::string signature = foo->constructors()[0]->signature_description();
    REQUIRE(signature.find("Foo(") == 0);
    REQUIRE(signature.find('?') != std::string::npos);
}

// a method whose parameter failed to parse used to clear args for the body-pass rebuild and then
// rank the candidate with implicit_arg_count() == 1 against an empty vector. the live declaration
// keeps its previous signature until replace_args; the lock is that ranking reports, not SIGSEGV,
// and not "too many positional arguments"
TEST_CASE("a method with a failed parameter can still be ranked", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo {\n"
        "    function bar(int32);\n"
        "}\n"
        "Foo $f = Foo();\n"
        "$f->bar(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE_FALSE(has_issue_containing(*bundle, "too many positional"));
}

// a closure whose `(` is missing after a capture list used to return without replace_args, so
// implicit_arg_count() was 1 against an empty args. `function [` is what starts_closure_literal
// accepts without a following `(`, so this is the path, not a bare `function {`
TEST_CASE("a closure missing its parameter list still has its environment", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32()> $f = function[] { return 1; };\n");

    REQUIRE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    AST::FunctionDeclNode *body = nullptr;

    for (AST::FunctionDeclNode *decl : m.nodes.of_type<AST::FunctionDeclNode>()) {
        if (decl->is_closure) {
            body = decl;
            break;
        }
    }

    REQUIRE(body != nullptr);
    REQUIRE(body->implicit_arg_count() == 1);
    REQUIRE(body->args.size() == 1);
}

// a prefix whose `(` is missing used to skip remainder without replace_args. infix cannot
// reach that arm: the header only classifies infix when a `(` already follows the symbol.
// the lock is that ranking sees a seated (empty) list, not a later SIGSEGV
TEST_CASE("a prefix operator missing its operand list still reports", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "operator !! : bool { return false; }\n");

    REQUIRE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, AST::operator_function_name("!!", AST::OpFixity::t_prefix));

    REQUIRE_FALSE(decls.empty());
    REQUIRE(decls[0]->args.size() == decls[0]->implicit_arg_count());
}
