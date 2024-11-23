#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTBundle.h>
#include <AST/ASTDestruction.h>
#include <AST/ASTPlaceExpr.h>
#include <AST/ExprNode.h>
#include <AST/IfStatementNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ReturnNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

using namespace AST;

using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

namespace
{
    // an owning type whose destructor calls nothing: the harness has no stdlib module, and what these
    // tests assert is where the *calls to* the destructor land, not what its body does
    const char *k_buffer =
        "struct Buffer {\n"
        "    usize $tag;\n"
        "    ptr<uint8> $data;\n"
        "    destructor() { $this->data = null; }\n"
        "}\n";

    // every drop the pass inserted into a scope, in order. a drop is an ordinary call whose
    // declaration is a destructor - there is nothing else it could be, since no source can spell one
    std::vector<FunctionCallExprNode *> drops_in(ScopeNode &scope)
    {
        std::vector<FunctionCallExprNode *> found;

        for (auto &child : scope.children) {
            if (!child.has_type<FunctionCallExprNode>()) {
                continue;
            }

            auto &call = child.get<FunctionCallExprNode>();
            if (call.decl != nullptr && call.decl->is_destructor()) {
                found.push_back(&call);
            }
        }

        return found;
    }

    // the variable a drop is destroying: its receiver is `&<place>`, and for a whole-variable drop
    // that place is a plain variable read
    VarDeclNode *dropped_variable(FunctionCallExprNode *drop)
    {
        REQUIRE(drop->arguments.size() == 1);
        return place_root_of(drop->arguments[0]);
    }

    ScopeNode &body_of(Module &m, const std::string &name)
    {
        auto decls = decls_named(m, name);
        REQUIRE(decls.size() == 1);
        REQUIRE(decls[0]->body != nullptr);
        return *decls[0]->body;
    }
}

TEST_CASE("a scope's owning locals are dropped in reverse declaration order", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function f() : void {\n"
        "    $a = Buffer(1, null);\n"
        "    $b = Buffer(2, null);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto drops = drops_in(body_of(m, "f"));

    REQUIRE(drops.size() == 2);
    REQUIRE(dropped_variable(drops[0])->name_full() == "$b");
    REQUIRE(dropped_variable(drops[1])->name_full() == "$a");
}

TEST_CASE("a returned local is not dropped", "[ownership]")
{
    // the rule the whole feature rests on. a constructor's `$this` is a body-local of value type with
    // an implicit `return $this`, so a drop here would free every constructed value twice
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function make() : Buffer {\n"
        "    $b = Buffer(1, null);\n"
        "    return $b;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(drops_in(body_of(m, "make")).empty());
}

TEST_CASE("a constructor's `$this` is not dropped either", "[ownership]")
{
    // the same rule, reached through the synthesized field-wise constructor rather than through
    // anything the user wrote - and it is the case that would have double-freed every value in the
    // language, so it gets an assertion of its own
    auto bundle = EchoTests::tests_make_parsed_bundle(k_buffer);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto ctors = decls_named(m, "Buffer");
    REQUIRE(ctors.size() == 1);
    REQUIRE(ctors[0]->body != nullptr);

    REQUIRE(drops_in(*ctors[0]->body).empty());
}

TEST_CASE("a moved-from local is not dropped, and its destination is", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function f() : void {\n"
        "    $a = Buffer(1, null);\n"
        "    $b = mv $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto drops = drops_in(body_of(m, "f"));

    REQUIRE(drops.size() == 1);
    REQUIRE(dropped_variable(drops[0])->name_full() == "$b");
}

TEST_CASE("a `mv` marker never survives the pass", "[ownership]")
{
    // like `:$`, `mv` is a marker that changes what a later pass does and is then gone. one reaching
    // codegen means a move was never resolved, and the copy it was meant to replace is still there -
    // which the codegen visitor throws about rather than compiling
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function f() : void {\n"
        "    $a = Buffer(1, null);\n"
        "    $b = mv $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the marker was created, so it is in the arena - which owns everything ever built and never
    // releases anything. what matters is that nothing in the tree still points at it
    REQUIRE(m.nodes.of_type<MoveExprNode>().size() == 1);

    auto &body = body_of(m, "f");
    bool checked_the_move = false;

    for (auto &child : body.children) {
        if (!child.has_type<VarDeclNode>()) {
            continue;
        }

        auto &decl = child.get<VarDeclNode>();
        if (decl.init_expr == nullptr) {
            continue;
        }

        REQUIRE(decl.init_expr->get_node_type() != NodeType::n_expr_move);

        if (decl.name_full() == "$b") {
            // erased down to the place it wrapped, which is the whole of what `mv` lowers to
            REQUIRE(decl.init_expr->get_node_type() == NodeType::n_varref);
            checked_the_move = true;
        }
    }

    REQUIRE(checked_the_move);
}

TEST_CASE("a by-value owning parameter is dropped at the end of the body", "[ownership]")
{
    // "$items is ours; it is destroyed at the end of this body". the receiver of a member is a
    // borrow, so it is filtered out by needs_destruction without a special case
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function consume(mv Buffer $b) : void { }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto drops = drops_in(body_of(m, "consume"));

    REQUIRE(drops.size() == 1);
    REQUIRE(dropped_variable(drops[0])->name_full() == "$b");
}

TEST_CASE("a borrow parameter owns nothing and is not dropped", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function peek(Buffer& $b) : usize { return $b->tag; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(drops_in(body_of(m, "peek")).empty());
}

TEST_CASE("an owning property is destroyed member-wise, with no destructor declared", "[ownership]")
{
    // "a struct that contains an owner is itself an owner, and nothing needs to be declared for that."
    // the drop for `$w` reaches into the property, which is what the receiver's place shape shows
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "struct Wrap { Buffer $inner; usize $version; }\n"
        "function f() : void {\n"
        "    $w = Wrap(Buffer(1, null), 2);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto drops = drops_in(body_of(m, "f"));

    // Wrap has no destructor of its own, so the only drop is its property's
    REQUIRE(drops.size() == 1);
    REQUIRE(drops[0]->decl->owner_type == &type_named(m, "Buffer")->complex_type());

    // and it is reached through the field, not through the whole variable
    REQUIRE(drops[0]->arguments.size() == 1);
    auto *addr = drops[0]->arguments[0];
    REQUIRE(addr->get_node_type() == NodeType::n_expr_addrof);
    REQUIRE(static_cast<AddrOfExprNode *>(addr)->operand->get_node_type() == NodeType::n_member_access);
}

TEST_CASE("a `return` drops every enclosing scope, innermost first", "[ownership]")
{
    // a return leaves all of them at once. `return` is the only early exit in the language today,
    // which is why there are exactly two insertion points
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function f(bool $early) : usize {\n"
        "    $outer = Buffer(1, null);\n"
        "    if ($early) {\n"
        "        $inner = Buffer(2, null);\n"
        "        return 10;\n"
        "    }\n"
        "    return 20;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto &body = body_of(m, "f");

    // the outer return drops the outer local
    auto outer_drops = drops_in(body);
    REQUIRE(outer_drops.size() == 1);
    REQUIRE(dropped_variable(outer_drops[0])->name_full() == "$outer");

    // and the branch's return drops the inner local *then* the outer one
    ScopeNode *if_scope = nullptr;
    for (auto &child : body.children) {
        if (child.has_type<IfStatementNode>()) {
            if_scope = child.get<IfStatementNode>().if_scope;
        }
    }

    REQUIRE(if_scope != nullptr);

    auto inner_drops = drops_in(*if_scope);
    REQUIRE(inner_drops.size() == 2);
    REQUIRE(dropped_variable(inner_drops[0])->name_full() == "$inner");
    REQUIRE(dropped_variable(inner_drops[1])->name_full() == "$outer");
}

TEST_CASE("an owning value that owns nothing needs no drops at all", "[ownership]")
{
    // the pass costs a program with no owning types exactly nothing, which is what keeps every
    // existing test's tree unchanged
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { int32 $x; int32 $y; }\n"
        "function f() : void {\n"
        "    $p = Point(1, 2);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(drops_in(body_of(m, "f")).empty());
}

TEST_CASE("the copy and move rules report what they cannot do", "[ownership]")
{
    SECTION("an implicit copy of an owning type") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function f() : void {\n"
            "    $a = Buffer(1, null);\n"
            "    $b = $a;\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be copied implicitly"));
    }

    SECTION("reading a moved-from local") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function f() : usize {\n"
            "    $a = Buffer(1, null);\n"
            "    $b = mv $a;\n"
            "    return $a->tag;\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "'$a' has been moved out of"));
    }

    SECTION("a `mv` parameter given an unmarked place") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function consume(mv Buffer $b) : void { }\n"
            "function f() : void {\n"
            "    $a = Buffer(1, null);\n"
            "    consume($a);\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "takes ownership of this argument"));
    }

    SECTION("a conditional move, which nothing would destroy on the other branch") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function consume(mv Buffer $b) : void { }\n"
            "function f(bool $flag) : void {\n"
            "    $a = Buffer(1, null);\n"
            "    if ($flag) { consume(mv $a); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "moved out of on only one branch"));
    }

    SECTION("a move inside a loop, which would run twice") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function consume(mv Buffer $b) : void { }\n"
            "function f() : void {\n"
            "    $a = Buffer(1, null);\n"
            "    $i = 0;\n"
            "    while ($i < 3) { consume(mv $a); $i++; }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "moved out of inside a loop"));
    }

    SECTION("moving a field out of a value") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "struct Wrap { Buffer $inner; }\n"
            "function f() : void {\n"
            "    $w = Wrap(Buffer(1, null));\n"
            "    $b = mv $w->inner;\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "can only move a whole variable"));
    }

    SECTION("`mv` on something with no storage to empty") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function make() : int32 { return 1; }\n"
            "function f() : void {\n"
            "    $a = mv make();\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "needs an expression with storage to move out of"));
    }
}

TEST_CASE("a value that owns nothing may still be moved, and may still be copied", "[ownership]")
{
    // "moving a value that owns nothing costs exactly what copying it costs." `mv` still makes the
    // source unset - the bookkeeping is uniform - but a plain copy of it stays perfectly legal
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { int32 $x; int32 $y; }\n"
        "function f() : void {\n"
        "    $a = Point(1, 2);\n"
        "    $b = $a;\n"
        "    $c = mv $b;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}
