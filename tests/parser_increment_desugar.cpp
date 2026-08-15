#include <catch2/catch_test_macros.hpp>

#include <AST/AssignNode.h>
#include <AST/ExprNode.h>
#include <AST/VarDeclNode.h>
#include <AST/VarRefNode.h>

#include "helpers.h"

// `$i++` is a statement, never an expression: it is desugared in the parser into the AssignNode
// the language already has, so pointer arithmetic, value coercion and the const check keep one
// implementation each (todo B10). the shape is pinned here because the desugar is invisible from
// a program's output - `$i = $i + 1` and `$i++` print the same thing either way

using namespace AST;

namespace
{
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

TEST_CASE("an index increment binds the address once", "[parser][pointer]")
{
    // `$b[0]++` must not parse the bracket twice: PointerAdjuster would visit a shared
    // subtree twice, and `operator []` would run for both the read and the write
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { int32 $v; }\n"
        "operator (Bag& $b)[usize $i] : int32& { return &$b->v; }\n"
        "$b = Bag(1);\n"
        "$b[0]++;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    VarDeclNode *tmp = nullptr;
    for (auto *decl : module.nodes.of_type<VarDeclNode>()) {
        if (decl->name_full() == "$__inc0") {
            tmp = decl;
            break;
        }
    }

    REQUIRE(tmp != nullptr);
    REQUIRE(tmp->init_expr != nullptr);
    REQUIRE(tmp->init_expr->get_node_type() == NodeType::n_expr_addrof);

    // PointerAdjuster inserts a deref on each read of the borrow, so the assign
    // is `*$__inc0 = *$__inc0 + 1` - two DerefExprNodes, not two VarRefs
    auto place_of = [](ExprNode *expr) -> ExprNode * {
        if (expr != nullptr && expr->get_node_type() == NodeType::n_expr_deref) {
            return static_cast<DerefExprNode *>(expr)->operand;
        }
        return expr;
    };

    AssignNode *inc = nullptr;
    for (auto *assign : module.nodes.of_type<AssignNode>()) {
        auto *place = place_of(assign->target);
        if (place == nullptr || place->get_node_type() != NodeType::n_varref) {
            continue;
        }

        auto *ref = static_cast<VarRefNode *>(place);
        if (ref->is_var() && &ref->get_var().decl() == tmp) {
            inc = assign;
            break;
        }
    }

    REQUIRE(inc != nullptr);
    REQUIRE(inc->value_expr != nullptr);
    REQUIRE(inc->value_expr->get_node_type() == NodeType::n_expr_binary);

    auto *binary = static_cast<BinaryExprNode *>(inc->value_expr);
    REQUIRE(binary->lhs != inc->target);
    REQUIRE(place_of(binary->lhs) != nullptr);
    REQUIRE(place_of(binary->lhs)->get_node_type() == NodeType::n_varref);
}
