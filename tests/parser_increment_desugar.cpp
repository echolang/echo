#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/AssignNode.h>
#include <AST/ExprNode.h>

// `$i++` is a statement, never an expression: it is desugared in the parser into the AssignNode
// the language already has, so pointer arithmetic, value coercion and the const check keep one
// implementation each (todo B10). the shape is pinned here because the desugar is invisible from
// a program's output - `$i = $i + 1` and `$i++` print the same thing either way

using namespace AST;

namespace {
    AssignNode *first_assign(Bundle &bundle)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *node : module.nodes.of_type<AssignNode>()) {
            return node;
        }
        return nullptr;
    }
}

TEST_CASE("'++' on a variable desugars to an assignment of a binary add", "[parser][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("$i = 5; $i++;");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *assign = first_assign(*bundle);
    REQUIRE(assign != nullptr);
    REQUIRE(assign->target->get_node_type() == NodeType::n_varref);
    REQUIRE(assign->value_expr->get_node_type() == NodeType::n_expr_binary);

    auto *binary = static_cast<BinaryExprNode *>(assign->value_expr);
    REQUIRE(binary->op_node->op->type == Token::Type::t_op_add);
    REQUIRE(binary->rhs->get_node_type() == NodeType::n_literal_int);

    // the operand is a second tree, not the target node under two parents. the pointer adjuster
    // rewrites edges in place, so a shared subtree would be visited - and rewritten - twice
    REQUIRE(binary->lhs != assign->target);

    // and it is not an initialization, so the const check still applies to it
    REQUIRE_FALSE(assign->is_initialization);
}

TEST_CASE("'--' steps the other way", "[parser][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("$i = 5; $i--;");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *assign = first_assign(*bundle);
    REQUIRE(assign != nullptr);

    auto *binary = static_cast<BinaryExprNode *>(assign->value_expr);
    REQUIRE(binary->op_node->op->type == Token::Type::t_op_sub);
}

TEST_CASE("the step is typed from the storage it moves", "[parser][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("int8 $b = 1; $b++;");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *assign = first_assign(*bundle);
    REQUIRE(assign != nullptr);

    auto *binary = static_cast<BinaryExprNode *>(assign->value_expr);
    REQUIRE(binary->rhs->result_type() == ValueType(ValueTypePrimitive::t_int8));
}

TEST_CASE("'$p:$++' re-seats rather than writing through", "[parser][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("int $a = 1; ptr<int> $p = &$a; $p:$++;");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *assign = first_assign(*bundle);
    REQUIRE(assign != nullptr);

    // the peel marker is erased by the adjuster, which leaves the pointer's own slot as the
    // target - a write here moves the pointer instead of the value it stands for
    REQUIRE(assign->target->get_node_type() == NodeType::n_varref);
    REQUIRE(assign->target->result_type().is_pointer());
}

TEST_CASE("'++' needs storage to step", "[parser][pointer]")
{
    // `$p:$:$` is the address of the pointer slot, not the slot
    auto bundle = EchoTests::tests_make_parsed_bundle("int $a = 1; ptr<int> $p = &$a; $p:$:$++;");
    REQUIRE(bundle->collector.has_critical_issues());
}
