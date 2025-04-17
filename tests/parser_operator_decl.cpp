#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTOperatorSemantics.h>
#include <AST/ASTOps.h>
#include <AST/FunctionDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::count_issues_containing;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;

namespace
{
    const char *k_point =
        "struct Point { float64 $x; }\n"
        "operator (Point $a) + (Point $b): Point { return Point($a->x + $b->x); }\n"
        "operator (Point $a) + (int32 $b): Point { return Point($a->x + $b); }\n";

    const Operator *op_named(const AST::Bundle &bundle, const std::string &spelling)
    {
        return bundle.collector.operators.get_operator(spelling);
    }
}

// **where in the parse passes an operator is published**, which no end-to-end case can see. the
// symbol goes in during the *type-name* pass, a whole pass earlier than any other declaration
// publishes anything, and the signature during the declaration pass. a refactor collapsing those two
// would leave every tests_eco case green and only this file red
TEST_CASE("an operator declaration publishes its symbol and its signature", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_point);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    const Operator *plus = op_named(*bundle, "+");
    REQUIRE(plus != nullptr);

    // `+` is a predefined operator, so declaring an overload of it must not mint a second, shadowing
    // symbol - `build_incdec_value` looks `"+"` up by string to desugar every `$i++`
    REQUIRE_FALSE(plus->is_custom());
    REQUIRE(plus->has_fixity(OpFixity::t_infix));
    REQUIRE(plus->is_declared());

    // both overloads are in *one* set, under the decorated name, and each is there once despite two
    // passes reaching the declaration
    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, operator_function_name("+", OpFixity::t_infix));

    REQUIRE(decls.size() == 2);

    for (const auto *decl : decls) {
        REQUIRE(decl->member_kind == MemberKind::t_operator);

        // a *free* declaration in every structural sense, exactly like a constructor: no owner type,
        // so no receiver is counted and no owner segment reaches the mangled name
        REQUIRE(decl->owner_type == nullptr);
        REQUIRE(decl->implicit_arg_count() == 0);
        REQUIRE(decl->args.size() == 2);
    }
}

TEST_CASE("a custom symbol is declared with its fixity and precedence", "[operator_decl]")
{
    SECTION("an infix word operator, defaulted precedence")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (float64 $a) avg (float64 $b): float64 { return ($a + $b) / 2.0; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *avg = op_named(*bundle, "avg");
        REQUIRE(avg != nullptr);
        REQUIRE(avg->is_custom());
        REQUIRE(avg->has_fixity(OpFixity::t_infix));
        REQUIRE_FALSE(avg->precedence_declared);
        REQUIRE(avg->precedence.sequence == CUSTOM_OP_DEFAULT_PRECEDENCE);
    }

    SECTION("a declared precedence clause is read, not skipped")
    {
        // the deleted lexer prepass stepped over this clause as a scope and never read it, so every
        // custom operator got the same hardcoded precedence
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator(35, right) (float64 $a) avg (float64 $b): float64 { return $a; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *avg = op_named(*bundle, "avg");
        REQUIRE(avg != nullptr);
        REQUIRE(avg->precedence_declared);
        REQUIRE(avg->precedence.sequence == 35);
        REQUIRE(avg->precedence.assoc == OpAssociativity::right);
    }

    SECTION("a prefix symbol spelled out of two tokens")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Flag { bool $on; }\n"
            "operator !!(Flag $f): bool { return $f->on; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *bang = op_named(*bundle, "!!");
        REQUIRE(bang != nullptr);
        REQUIRE(bang->is_custom());
        REQUIRE(bang->has_fixity(OpFixity::t_prefix));
        REQUIRE_FALSE(bang->has_fixity(OpFixity::t_infix));
    }

    SECTION("a suffix symbol")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Distance { uint64 $mm; }\n"
            "operator (uint64 $a)mm: Distance { return Distance($a); }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *mm = op_named(*bundle, "mm");
        REQUIRE(mm != nullptr);
        REQUIRE(mm->has_fixity(OpFixity::t_suffix));
        REQUIRE_FALSE(mm->has_fixity(OpFixity::t_infix));
    }

    SECTION("one symbol may be declared in more than one position")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) blah (S $b): int32 { return 1; }\n"
            "operator blah(S $a): int32 { return 2; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *blah = op_named(*bundle, "blah");
        REQUIRE(blah != nullptr);
        REQUIRE(blah->has_fixity(OpFixity::t_infix));
        REQUIRE(blah->has_fixity(OpFixity::t_prefix));

        // and the two are separate overload sets, because a prefix and a suffix declaration of one
        // symbol are otherwise the same signature
        auto &m = bundle->modules.find_module("test");
        REQUIRE(decls_named(m, operator_function_name("blah", OpFixity::t_infix)).size() == 1);
        REQUIRE(decls_named(m, operator_function_name("blah", OpFixity::t_prefix)).size() == 1);
    }
}

// **the three ways a declaration can be positioned relative to its use site**, all of which have to
// work and none of which an end-to-end case distinguishes from the others. these are the cases a
// regression would silently take away
TEST_CASE("an operator is declared anywhere and valid everywhere", "[operator_decl]")
{
    SECTION("declared below its use site, and its type below the operator")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "$a = Point(1.0);\n"
            "$b = Point(2.0);\n"
            "$c = $a + $b;\n"
            "operator (Point $p) + (Point $q): Point { return Point($p->x + $q->x); }\n"
            "struct Point { float64 $x; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        REQUIRE(EchoTests::calls_to(m, operator_function_name("+", OpFixity::t_infix)).size() == 1);
    }

    SECTION("declared in another file")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
            "$a = Point(1.0);\n"
            "$b = Point(2.0);\n"
            "$c = $a + $b;\n",

            "struct Point { float64 $x; }\n"
            "operator (Point $p) + (Point $q): Point { return Point($p->x + $q->x); }\n",
        });

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("used inside a struct property initializer")
    {
        // the expression the *declaration* pass parses. it is the whole reason the symbol is published
        // one pass earlier: asking the overload set here would answer differently depending on which
        // file, and which declaration inside it, had been walked first
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Distance { uint64 $mm = 1m; }\n"
            "operator (uint64 $a)m: uint64 { return $a * 1000; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }
}

TEST_CASE("a declaration that could never be reached is refused", "[operator_decl]")
{
    SECTION("inside a struct")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; operator (S $a) + (S $b): S { return $a; } }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be declared inside a struct"));
    }

    SECTION("inside a block")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "{ operator (S $a) + (S $b): S { return $a; } }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be declared inside a block"));
    }

    SECTION("a keyword as the symbol")
    {
        // matching happens on token *values*, so a symbol spelled `if` would turn every `if` in the
        // program into an operator
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a) if (int32 $b): int32 { return 1; }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be part of an operator symbol"));

        // ...and reported once, though every pass reaches the declaration
        REQUIRE(count_issues_containing(*bundle, "cannot be part of an operator symbol") == 1);
    }

    SECTION("a built-in symbol over nothing but primitives")
    {
        // it would register, mangle and be emitted, and then never fire, because the built-in meaning
        // wins for two primitives. the class of silent no-op publish_implicit_conversion refuses seven
        // shapes for
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a) + (int32 $b): int32 { return 1; }\n");

        REQUIRE(has_issue_containing(*bundle, "would never be used"));
    }

    SECTION("...but a custom symbol over primitives is exactly the point")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a) avg (int32 $b): int32 { return ($a + $b) / 2; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("...and so is an operator over a concrete instantiation")
    {
        // the grammar gives an operator no name for a `<T>` to follow, so a *generic* operator cannot
        // be written at all - todo/A32. one over a concrete instantiation is an ordinary declaration
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Vec<T> { T $first; }\n"
            "operator (Vec<int32> $a) + (Vec<int32> $b): Vec<int32> {\n"
            "    return Vec<int32>($a->first + $b->first);\n"
            "}\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("a suffix increment, which is a statement")
    {
        // `$i++;` never reaches parse_expr - ScopeParser routes it straight to parse_varexpr, which
        // desugars it - so a declaration of it would be silently ignored
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a)++: int32 { return $a; }\n");

        REQUIRE(has_issue_containing(*bundle, "is a statement"));
    }

    SECTION("assignment, which is also a statement")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) = (S $b): S { return $a; }\n");

        REQUIRE(has_issue_containing(*bundle, "assignment is a statement"));
    }

    SECTION("a void return")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) sum (S $b): void { return; }\n");

        REQUIRE(has_issue_containing(*bundle, "An operator is an expression"));
    }

    SECTION("one symbol declared both infix and suffix")
    {
        // `$a blah $b` could not say whether the symbol closes the left operand or opens the right
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) blah (S $b): int32 { return 1; }\n"
            "operator (S $a) blah : int32 { return 2; }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be both infix and suffix"));
    }

    SECTION("two different precedences for one symbol")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator(30, left) (S $a) op (S $b): int32 { return 1; }\n"
            "operator(50, left) (S $a) op (S $b): int32 { return 2; }\n");

        REQUIRE(has_issue_containing(*bundle, "A symbol binds one way everywhere"));
    }

    SECTION("a precedence on an overload of a built-in symbol")
    {
        // `+` binds the way the language says it binds, whatever anybody overloads it for, or two
        // files would parse one expression differently
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator(30, left) (S $a) + (S $b): S { return $a; }\n");

        REQUIRE(has_issue_containing(*bundle, "already has a precedence"));
    }
}

TEST_CASE("a use site becomes a call only where there is no built-in meaning", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { float64 $x; }\n"
        "operator (Point $a) + (Point $b): Point { return Point($a->x + $b->x); }\n"
        "$p = Point(1.0);\n"
        "$q = Point(2.0);\n"
        "$r = $p + $q;\n"
        "$n = 1 + 2;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the struct operands resolved to the declaration...
    auto calls = EchoTests::calls_to(m, operator_function_name("+", OpFixity::t_infix));
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);

    // ...while `1 + 2` is still an ordinary binary expression, not a call. the declaration exists, so
    // this is the predicate deciding rather than the absence of an overload set
    REQUIRE(EchoTests::tests_make_node_description_expr("1 + 2;")
        == "binexp<int32>(literal<int32>(1) + literal<int32>(2))");
}

// the decorated name is unspellable by design, and it has to survive being an llvm symbol
TEST_CASE("an operator's decorated name is unspellable and manglable", "[operator_decl]")
{
    REQUIRE(operator_function_name("+", OpFixity::t_infix) == "operator +");
    REQUIRE(operator_function_name("!!", OpFixity::t_prefix) == "operator prefix !!");
    REQUIRE(operator_function_name("mm", OpFixity::t_suffix) == "operator suffix mm");

    // the space is what makes it unspellable: no identifier may contain one
    for (const auto &name : {
        operator_function_name("+", OpFixity::t_infix),
        operator_function_name("avg", OpFixity::t_infix)}) {
        REQUIRE(name.find(' ') != std::string::npos);
    }

    // ...and the mangled form holds nothing an assembler could object to
    const std::string mangled = mangle_operator_name(operator_function_name("+", OpFixity::t_infix));

    REQUIRE(mangled == "operatorx20x2b");

    for (const char c : mangled) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
        REQUIRE(safe);
    }
}
