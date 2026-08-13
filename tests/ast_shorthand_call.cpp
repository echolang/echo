#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::type_named;

namespace
{
    // the Echo the cases below share: a type with two statics, and a free function taking one of it
    const char *POINT_SOURCE =
        "struct Point {\n"
        "    float64 $x;\n"
        "    static function origin() : Point { return Point(0.0); }\n"
        "    static function of(float64 $v) : Point { return Point($v); }\n"
        "}\n"
        "function draw(Point $p) : float64 { return $p->x; }\n";
}

TEST_CASE("a shorthand takes its owner from an argument's parameter", "[shorthand]")
{
    // the one destination the parser cannot reach: a parameter's type sits on a declaration nobody
    // has chosen when the argument is read, so AST::CallResolver binds it instead - beside bind_null_to
    // and for the same reason
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(POINT_SOURCE) + "$r = draw(.origin());\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    auto calls = calls_to(m, "origin");
    REQUIRE(calls.size() == 1);

    // it stays marked as a shorthand after binding: what that records is how it was *written*, which
    // is what a diagnostic needs to word a remedy
    REQUIRE(calls[0]->is_shorthand_static_call());
    REQUIRE(calls[0]->static_owner.has_complex_type());
    REQUIRE(calls[0]->decl != nullptr);
}

TEST_CASE("a shorthand takes its owner from a return type", "[shorthand]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(POINT_SOURCE) + "function make() : Point { return .of(2.0); }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "of");

    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
}

TEST_CASE("a shorthand takes its owner from a declared variable's type", "[shorthand]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(POINT_SOURCE) + "Point $p = .of(3.0);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "of");

    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
}

TEST_CASE("an unbound shorthand answers void, which is what stops it choosing an overload", "[shorthand]")
{
    // **the property the whole design rests on, and it needs no code.** a call with no decl already
    // answers `void` from result_type(), which is_undetermined_type accepts - so argument_fit scores a
    // shorthand t_undetermined at its first arm and strictly_better skips it on both sides
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(POINT_SOURCE) + "$r = draw(.nope());\n");

    // it does not resolve, and the message is about the missing type rather than the missing name
    REQUIRE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "nope");
    REQUIRE(calls.size() == 1);

    // the owner was named - `draw` has one candidate, so it was chosen without consulting types - and
    // Point simply declares no `nope`
    REQUIRE(calls[0]->is_shorthand_static_call());
    REQUIRE(calls[0]->decl == nullptr);
}

TEST_CASE("a nested shorthand binds from the outer call's parameter", "[shorthand]")
{
    // `return .error(.timeout(30));` resolves outside-in: the return type names the outer's owner,
    // settling the outer gives its parameter types, and the inner's owner is one of those. no step
    // ever needs a type it is still computing
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct err {\n"
        "    int32 $code;\n"
        "    static function timeout(int32 $s) : err { return err($s); }\n"
        "}\n"
        "struct box {\n"
        "    err $e;\n"
        "    static function of(err $e) : box { return box($e); }\n"
        "}\n"
        "function build() : box { return .of(.timeout(30)); }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    auto outer = calls_to(m, "of");
    REQUIRE(outer.size() == 1);
    REQUIRE(outer[0]->decl != nullptr);

    auto inner = calls_to(m, "timeout");
    REQUIRE(inner.size() == 1);
    REQUIRE(inner[0]->decl != nullptr);
    REQUIRE(inner[0]->is_shorthand_static_call());
}

TEST_CASE("a shorthand with no destination is refused at the dot", "[shorthand]")
{
    EchoTests::assert_code_emits_issue(
        std::string(POINT_SOURCE) + "$x = .origin();\n",
        "'.origin(...)' takes its type from where its value goes, and nothing here says what that is"
    );
}

TEST_CASE("a shorthand never picks between overloads", "[shorthand]")
{
    // and the refusal is its own kind, because AmbiguousCall's remedy - cast the argument - is the one
    // thing that cannot work on a value with no type to cast from
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    float64 $x;\n"
        "    static function norm(float64 $a) : Point { return Point($a); }\n"
        "}\n"
        "struct Circle {\n"
        "    float64 $r;\n"
        "    static function norm(float64 $a) : Circle { return Circle($a); }\n"
        "}\n"
        "function draw(Point $p) : float64 { return $p->x; }\n"
        "function draw(Circle $c) : float64 { return $c->r; }\n"
        "$r = draw(.norm(2.0));\n");

    REQUIRE(bundle->collector.has_critical_issues());

    // the tie is refused as its own kind, and both overloads are named so the reader can see what
    // they have to choose between
    REQUIRE(EchoTests::has_issue_containing(
        *bundle, "The overload of 'draw' cannot be chosen: '.norm(...)' has no type of its own"));
    REQUIRE(EchoTests::has_issue_containing(*bundle, "draw(Point)"));
    REQUIRE(EchoTests::has_issue_containing(*bundle, "draw(Circle)"));

    // and exactly once: the enclosing call speaks first and marks the shorthand terminal, so the
    // finalizing sweep does not go on to report that nothing named its owner
    REQUIRE(bundle->collector.issues.size() == 1);
}
