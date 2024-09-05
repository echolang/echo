#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTNodeReference.h>
#include <AST/ASTPlaceExpr.h>
#include <AST/ExprNode.h>
#include <AST/LiteralValueNode.h>
#include <AST/MemberAccessNode.h>
#include <AST/NullNode.h>
#include <AST/VarDeclNode.h>
#include <AST/VarNode.h>
#include <AST/VarRefNode.h>

// AST::is_place_expression - "does this expression denote storage".
//
// four consumers have to agree on the answer: the parser rejecting `&($a + $b)`, the adjustment
// pass deciding value versus place position, the type checker locating a diagnostic, and the
// lvalue codegen's dispatch. when each kept its own switch they drifted, and member reads ended
// up disagreeing with member writes (todo/B4). pinning the exact tag set here makes widening it a
// deliberate act rather than a side effect.

using namespace AST;

namespace {
    // a parsed module gives us real nodes of each kind to ask about, rather than hand-built ones
    // whose edges would not match what the parser actually produces
    template <typename T>
    T *first_of(Bundle &bundle)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *node : module.nodes.of_type<T>()) {
            return node;
        }
        return nullptr;
    }
}

TEST_CASE("The four place kinds denote storage", "[AST][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int $a; int $b; }\n"
        "$s = P(1, 2);\n"
        "ptr<int> $p = &$s->a;\n"
        "echo $p:$[1];\n"
        "echo $p;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *var_ref = first_of<VarRefNode>(*bundle);
    auto *member = first_of<MemberAccessNode>(*bundle);
    auto *index = first_of<IndexExprNode>(*bundle);
    auto *deref = first_of<DerefExprNode>(*bundle);

    REQUIRE(var_ref != nullptr);
    REQUIRE(member != nullptr);
    REQUIRE(index != nullptr);
    REQUIRE(deref != nullptr);

    REQUIRE(is_place_expression(*var_ref));
    REQUIRE(is_place_expression(*member));
    REQUIRE(is_place_expression(*index));
    REQUIRE(is_place_expression(*deref));
}

TEST_CASE("An address, a literal and a call result are not places", "[AST][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function get() : int { return 1; }\n"
        "$a = 5;\n"
        "ptr<int> $p = &$a;\n"
        "echo get();\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *addr = first_of<AddrOfExprNode>(*bundle);
    auto *literal = first_of<LiteralIntExprNode>(*bundle);
    auto *call = first_of<FunctionCallExprNode>(*bundle);

    REQUIRE(addr != nullptr);
    REQUIRE(literal != nullptr);
    REQUIRE(call != nullptr);

    // `&$a` yields an address but has none of its own - which is why `&&$a` and `&get()` are
    // rejected rather than silently inventing storage
    REQUIRE_FALSE(is_place_expression(*addr));
    REQUIRE_FALSE(is_place_expression(*literal));
    REQUIRE_FALSE(is_place_expression(*call));
}

TEST_CASE("value_result_type reads through a place and leaves a non-place alone", "[AST][pointer]")
{
    // the rule behind two inferences that look alike but are not: `$copy = $r` over an `int32&`
    // infers int32 because reading a place auto-derefs, while `$ref = &$var` infers int32&
    // because an address-of is already the value it means
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$var = 10;\n"
        "int& $r = &$var;\n"
        "$copy = $r;\n"
        "$alias = &$var;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    for (auto *decl : module.nodes.of_type<VarDeclNode>()) {
        if (decl->name_full() == "$copy") {
            REQUIRE(decl->type().is_primitive_of_type(ValueTypePrimitive::t_int32));
        }
        if (decl->name_full() == "$alias") {
            REQUIRE(decl->type().is_pointer());
            REQUIRE_FALSE(decl->type().is_nullable());
        }
    }
}

TEST_CASE("A peel is not a place, so its own address cannot be taken twice over", "[AST][pointer]")
{
    // `$p:$` names the pointer, and the parser turns a second `:$` into an address-of rather than
    // nesting markers. a third has nothing left to address, which is the boundary of that rule
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$a = 1;\n"
        "ptr<int> $p = &$a;\n"
        "ptr<ptr<int>> $pp = &$p;\n"
        "echo ($pp:$:$:$ == null);\n");

    REQUIRE(bundle->collector.has_critical_issues());

    bool found = false;
    for (const auto &issue : bundle->collector.issues) {
        if (issue->message().find("':$' needs an expression with storage") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("null is deliberately not an expression node for conversion purposes", "[AST][pointer]")
{
    // NullNode is an ExprNode, but `n_null` is intentionally absent from is_expression_node().
    //
    // that predicate gates try_implicit_cast (src/Parser/ExprParser.cpp), and null must not be
    // wrapped in a TypeCastNode: it has no type of its own, it acquires one through bound_type,
    // and the type checker's null-specific rules all test for the raw n_null tag. wrapping it
    // would hide the tag and turn "cannot be null" into a silent conversion.
    //
    // pinned because the omission looks exactly like the bug CLAUDE.md's "Adding an AST node"
    // step 9 warns about, and would otherwise be "fixed" into a regression
    auto bundle = EchoTests::tests_make_parsed_bundle("ptr<int> $p = null;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *null_node = first_of<NullNode>(*bundle);
    REQUIRE(null_node != nullptr);
    REQUIRE_FALSE(make_ref(null_node).is_expression_node());

    // it still is not a place, so `&null` and `null:$` have nothing to reach
    REQUIRE_FALSE(is_place_expression(*null_node));

    // and the declaration bound it, which is how null gets a type at all
    REQUIRE(null_node->is_bound());
    REQUIRE(null_node->result_type().is_pointer());
}
