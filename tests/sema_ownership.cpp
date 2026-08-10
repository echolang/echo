#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <AST/ASTBundle.h>
#include <AST/ASTDestruction.h>
#include <AST/ASTMemberLookup.h>
#include <AST/AssignNode.h>
#include <AST/MemberAccessNode.h>
#include <AST/ASTPlaceExpr.h>
#include <AST/ExprNode.h>
#include <AST/IfStatementNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ReturnNode.h>
#include <AST/ScopeNode.h>
#include <AST/TemporaryBindExprNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

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
        "    destructor() { $this->data:$ = null; }\n"
        "}\n";

    // the drops in a node list, in order. a drop is an ordinary call whose declaration is a destructor -
    // there is nothing else it could be, since no source can spell one
    std::vector<FunctionCallExprNode *> drops_in_list(const NodeReferenceList &list)
    {
        std::vector<FunctionCallExprNode *> found;

        for (auto &child : list) {
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

    // every drop the pass inserted into a scope, in order
    std::vector<FunctionCallExprNode *> drops_in(ScopeNode &scope)
    {
        return drops_in_list(scope.children);
    }

    // the drops a `return` owes. they live *on* the return rather than as statements ahead of it,
    // because the returned expression may read what is being dropped - `return $c->x` over an owning
    // `$c` - so codegen has to compute the value first and unwind after
    std::vector<FunctionCallExprNode *> drops_on_return(ScopeNode &scope)
    {
        for (auto &child : scope.children) {
            if (child.has_type<ReturnNode>()) {
                return drops_in_list(child.get<ReturnNode>().unwind);
            }
        }

        return {};
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

    // the outer return drops the outer local. asked of the *return* rather than of the scope: a
    // return's drops ride on it, so that its own expression is evaluated before they run
    auto outer_drops = drops_on_return(body);
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

    auto inner_drops = drops_on_return(*if_scope);
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

// --- the copy constructor, and where the old value's teardown lands -------------------------------

namespace
{
    // the same owning type with a copy constructor, so a place arriving at a destination has somewhere
    // to go other than a diagnostic
    const char *k_copyable =
        "struct Box {\n"
        "    usize $tag;\n"
        "    ptr<uint8> $data;\n"
        "    constructor(usize $t, ptr<uint8> $d) { $this->tag = $t; $this->data:$ = $d; }\n"
        "    constructor(Box& $other) { $this->tag = $other->tag; $this->data:$ = $other->data; }\n"
        "    destructor() { $this->data:$ = null; }\n"
        "}\n";

    // the value a statement puts somewhere, or null when it puts none: the two statements the pass
    // rewrites the right-hand side of, asked the same way
    ExprNode *value_of(NodeReference &child)
    {
        if (child.has_type<VarDeclNode>()) {
            return child.get<VarDeclNode>().init_expr;
        }

        if (child.has_type<AssignNode>()) {
            return child.get<AssignNode>().value_expr;
        }

        return nullptr;
    }

    // every copy the pass inserted into a scope's declarations, in order. a copy is an ordinary call
    // whose declaration is the type's copy constructor, which is the same bargain a drop makes
    std::vector<FunctionCallExprNode *> copies_in(ScopeNode &scope)
    {
        std::vector<FunctionCallExprNode *> found;

        for (auto &child : scope.children) {
            ExprNode *value = value_of(child);

            if (value == nullptr || value->get_node_type() != NodeType::n_expr_call) {
                continue;
            }

            auto *call = static_cast<FunctionCallExprNode *>(value);
            if (call->decl != nullptr && call->decl->is_constructor()
                && call->decl == find_copy_constructor(call->decl->get_return_type().get_complex_type())) {
                found.push_back(call);
            }
        }

        return found;
    }

    AssignNode *first_assign_in(ScopeNode &scope)
    {
        for (auto &child : scope.children) {
            if (child.has_type<AssignNode>()) {
                return &child.get<AssignNode>();
            }
        }
        return nullptr;
    }
}

TEST_CASE("an implicit copy is a resolved call to the type's copy constructor", "[ownership]")
{
    // the same bargain every drop makes: the tree says what happens, so codegen needs nothing, --print ast-resolved
    // shows it, and the type checker validates what the pass inserted
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "function f() : void {\n"
        "    $a = Box(1, null);\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *expected = find_copy_constructor(&type_named(m, "Box")->complex_type());
    REQUIRE(expected != nullptr);

    auto copies = copies_in(body_of(m, "f"));
    REQUIRE(copies.size() == 1);

    // resolved at insertion: there is no name to look up and no overload set to search
    REQUIRE(copies[0]->decl == expected);

    // and the receiver is the address of the source place, exactly as a drop's is
    REQUIRE(copies[0]->arguments.size() == 1);
    REQUIRE(copies[0]->arguments[0]->get_node_type() == NodeType::n_expr_addrof);
    REQUIRE(place_root_of(copies[0]->arguments[0]) != nullptr);

    // the source stays live, which is the whole difference between a copy and a move: it is still
    // dropped at the end of its scope, alongside the copy
    REQUIRE(drops_in(body_of(m, "f")).size() == 2);
}

TEST_CASE("the explicit copy call resolves to the same declaration", "[ownership]")
{
    // the reason recognition beats a new spelling - asserted rather than assumed, since `Box($a)` was
    // an ordinary overload resolution long before any of this existed
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "function f() : void {\n"
        "    $a = Box(1, null);\n"
        "    $b = Box($a);\n"
        "    $c = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto copies = copies_in(body_of(m, "f"));

    REQUIRE(copies.size() == 2);
    REQUIRE(copies[0]->decl == copies[1]->decl);
}

TEST_CASE("a `mv` parameter given an unmarked place still errors, copy constructor or not", "[ownership]")
{
    // the annotation is a contract that the value is handed over. quietly copying instead would make it
    // mean nothing, so this check sits ahead of the copy hook and keeps winning
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "function consume(mv Box $taken) : void { }\n"
        "function f() : void {\n"
        "    $a = Box(1, null);\n"
        "    consume($a);\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "takes ownership of this argument"));
}

TEST_CASE("an assignment carries the old value's teardown instead of pushing it ahead", "[ownership]")
{
    // the drop used to be pushed into the statement list *before* the assignment, so a right-hand side
    // that read the target copied from a destroyed value. carried on the assignment, gen_assign orders
    // it: evaluate the right-hand side, tear the old value down, store
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "function f() : void {\n"
        "    $a = Box(1, null);\n"
        "    $a = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto &body = body_of(m, "f");

    auto *assign = first_assign_in(body);
    REQUIRE(assign != nullptr);
    REQUIRE(assign->teardown_old != nullptr);
    REQUIRE(assign->teardown_old->children.size() == 1);

    // and *not* in the statement list: the only drop standing on its own is the scope-exit one
    REQUIRE(drops_in(body).size() == 1);
}

TEST_CASE("a moved-from assignment target owes no teardown", "[ownership]")
{
    // what is still sitting in the slot belongs to whoever the value was handed to. re-seating it is
    // also not a use-after-move: the target is written, not read
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "function consume(mv Box $taken) : void { }\n"
        "function f() : void {\n"
        "    $a = Box(1, null);\n"
        "    consume(mv $a);\n"
        "    $a = Box(2, null);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *assign = first_assign_in(body_of(m, "f"));

    REQUIRE(assign != nullptr);
    REQUIRE(assign->teardown_old == nullptr);
}

TEST_CASE("the class path carries a flag rather than nodes", "[ownership]")
{
    // pinned deliberately. one concept - the old value's teardown - but a class's release cannot be a
    // node: it needs the old handle out of the slot codegen has already addressed, and a class target
    // may be a field or an element whose subscript must not be evaluated twice. a later tidy-up that
    // collapses the two has to argue with this test
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Res { usize $tag; }\n"
        "function f() : void {\n"
        "    $a = Res(1);\n"
        "    $a = Res(2);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *assign = first_assign_in(body_of(m, "f"));

    REQUIRE(assign != nullptr);
    REQUIRE(assign->releases_old);
    REQUIRE(assign->teardown_old == nullptr);
}

TEST_CASE("a constructor's write to its own `$this` is that field's first write", "[ownership]")
{
    // `$this` is a fresh zero-filled slot, so there is no old value owed a teardown - which is what
    // makes a copy constructor for a nested owner writable at all. deliberately not a
    // t_initialization *destination*, so a place source still reaches the copy hook rather than being
    // silently moved
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "struct Outer {\n"
        "    Box $inner;\n"
        "    constructor(usize $t) { $this->inner = Box($t, null); }\n"
        "    constructor(Outer& $other) { $this->inner = $other->inner; }\n"
        "}\n"
        "function f() : void {\n"
        "    $a = Outer(1);\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

// --- the copy the compiler writes itself ----------------------------------------------------------

namespace
{
    // a struct whose only owner is a class. nothing here says how it is copied, which is the point:
    // one more reference to each field is the only thing a copy could mean
    const char *k_holds_classes =
        "class Handle { int32 $id; }\n"
        "struct Pair {\n"
        "    Handle $a;\n"
        "    Handle $b;\n"
        "}\n";

    // the retains a synthesized body puts on the right of its field-wise assignments
    size_t retains_in(ScopeNode &scope)
    {
        size_t found = 0;

        for (auto &child : scope.children) {
            ExprNode *value = value_of(child);

            if (value != nullptr && value->get_node_type() == NodeType::n_expr_retain) {
                found++;
            }
        }

        return found;
    }
}

TEST_CASE("a struct whose owning fields are all classes is copied by a constructor nobody wrote", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_holds_classes) +
        "function f() : void {\n"
        "    $a = Pair(Handle(1), Handle(2));\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *synthesized = find_copy_constructor(&type_named(m, "Pair")->complex_type());
    REQUIRE(synthesized != nullptr);

    // indistinguishable from a written one from here on, which is the whole design: the copy is the
    // same resolved call, through the same hook, and nothing downstream asks who wrote the body
    auto copies = copies_in(body_of(m, "f"));
    REQUIRE(copies.size() == 1);
    REQUIRE(copies[0]->decl == synthesized);

    // one non-nullable borrow of its own type, so AST::is_copy_constructor would recognise it too
    REQUIRE(synthesized->args.size() == 1);
    REQUIRE(is_copy_constructor(synthesized, type_named(m, "Pair")->value_type()));
}

TEST_CASE("the synthesized copy retains each class field", "[ownership]")
{
    // the body it builds is a field-wise assignment and nothing else - no retain is written into it.
    // they appear because the next round walks those assignments through resolve_value_arrival, the
    // same arm a hand-written copy constructor's body goes through
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_holds_classes) +
        "function f() : void {\n"
        "    $a = Pair(Handle(1), Handle(2));\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *synthesized = find_copy_constructor(&type_named(m, "Pair")->complex_type());
    REQUIRE(synthesized != nullptr);
    REQUIRE(synthesized->body != nullptr);

    REQUIRE(retains_in(*synthesized->body) == 2);

    // and nothing it writes into is owed a teardown: `$this` is fresh storage, so the assignments are
    // initializations and there is no old reference to release
    for (auto &child : synthesized->body->children) {
        if (child.has_type<AssignNode>()) {
            REQUIRE(child.get<AssignNode>().is_initialization);
            REQUIRE_FALSE(child.get<AssignNode>().releases_old);

            // and deliberately not handed over: `$other` is a borrow, so its fields are copied
            REQUIRE_FALSE(child.get<AssignNode>().hands_over_value);
        }
    }
}

TEST_CASE("a struct that declares a destructor gets no synthesized copy", "[ownership]")
{
    // what a second value running that body would mean is the question its author has not answered,
    // and the compiler does not guess it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_holds_classes) +
        "struct Pool {\n"
        "    Handle $h;\n"
        "    destructor() { }\n"
        "}\n"
        "function f() : void {\n"
        "    $a = Pool(Handle(1));\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "cannot be copied implicitly"));

    auto &m = bundle->modules.find_module("test");
    REQUIRE(find_copy_constructor(&type_named(m, "Pool")->complex_type()) == nullptr);
}

TEST_CASE("an owner that is not a class gets no synthesized copy", "[ownership]")
{
    // the split worth stating: not "we cannot copy owners", but "we cannot copy owners we have no
    // rule for". ownership ends at a raw pointer, and the type holding it is the only thing that
    // knows what duplicating it would mean
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "function f() : void {\n"
        "    $a = Buffer(1, null);\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "cannot be copied implicitly"));

    auto &m = bundle->modules.find_module("test");
    REQUIRE(find_copy_constructor(&type_named(m, "Buffer")->complex_type()) == nullptr);
}

TEST_CASE("a written copy constructor is never replaced by a synthesized one", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Handle { int32 $id; }\n"
        "struct Pair {\n"
        "    Handle $a;\n"
        "    Handle $b;\n"
        "    constructor(Handle $x, Handle $y) { $this->a = $x; $this->b = $y; }\n"
        "    constructor(Pair& $other) { $this->a = $other->a; $this->b = $other->b; }\n"
        "}\n"
        "function f() : void {\n"
        "    $a = Pair(Handle(1), Handle(2));\n"
        "    $b = $a;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the declaration the parser published, still - and still in the overload set, so `Pair($a)`
    // and `$b = $a` remain one operation
    auto written = decls_named(m, "Pair");
    auto *found = find_copy_constructor(&type_named(m, "Pair")->complex_type());

    REQUIRE(found != nullptr);
    REQUIRE(std::find(written.begin(), written.end(), found) != written.end());
}

TEST_CASE("an owning field initialized twice in one constructor is reported", "[ownership]")
{
    // the first write's value would never be destroyed, and nothing further down could notice, because
    // both writes claim the storage was fresh
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_copyable) +
        "struct Outer {\n"
        "    Box $inner;\n"
        "    constructor(usize $t) { $this->inner = Box($t, null); $this->inner = Box($t, null); }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "is initialized twice"));
}

// --- a temporary with an owner -------------------------------------------------------
//
// `$o->get()->tag` reads a member off a value the callee handed back and nobody stored. the pass gives
// it storage for exactly as long as the expression reading it, and that storage needs an owner - which
// is why this lives here rather than in the pointer tests: the node exists for the *drop*

namespace
{
    // the one binding in a body, or null. nothing else in the pass creates one, so finding it by type
    // is finding the rewrite
    TemporaryBindExprNode *binding_in(Module &m)
    {
        for (auto *node : m.nodes.of_type<TemporaryBindExprNode>()) {
            return node;
        }
        return nullptr;
    }
}

TEST_CASE("a member of a call result is bound to a temporary", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Inner { int32 $tag; }\n"
        "struct Outer {\n"
        "    Inner $in;\n"
        "    function get() : Inner { return $this->in; }\n"
        "}\n"
        "function f(Outer& $o) : int32 { return $o->get()->tag; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *bind = binding_in(m);

    REQUIRE(bind != nullptr);
    REQUIRE(bind->temporaries.size() == 1);

    // the call *is* the initializer - the value moves into the slot rather than being copied into it
    VarDeclNode *temp = bind->temporaries[0];
    REQUIRE(temp->init_expr != nullptr);
    REQUIRE(temp->init_expr->get_node_type() == NodeType::n_expr_call);

    // and the body now reads out of that slot: the base is a place, which is the whole of what codegen
    // needed - it addresses a varref through the arm a named local uses
    REQUIRE(bind->body != nullptr);
    REQUIRE(bind->body->get_node_type() == NodeType::n_member_access);

    auto *access = static_cast<MemberAccessNode *>(bind->body);
    REQUIRE(place_root_of(access->get_base_node().unsafe_ptr<ExprNode>()) == temp);

    // `Inner` owns nothing, so there is nothing to tear down
    REQUIRE(bind->teardown.empty());
}

TEST_CASE("an owning temporary carries its own drop", "[ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::string(k_buffer) +
        "struct Outer {\n"
        "    Buffer $buf;\n"
        "    usize $tag;\n"
        "    function copy_of() : Buffer { return Buffer($this->tag, null); }\n"
        "}\n"
        "function f(Outer& $o) : usize { return $o->copy_of()->tag; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *bind = binding_in(m);

    REQUIRE(bind != nullptr);
    REQUIRE(bind->temporaries.size() == 1);

    // one drop, on the temporary itself, and it rides on the binding rather than on the enclosing
    // statement - which is what makes a temporary inside a loop condition die once per turn
    auto drops = drops_in_list(bind->teardown);
    REQUIRE(drops.size() == 1);
    REQUIRE(dropped_variable(drops[0]) == bind->temporaries[0]);
}

TEST_CASE("a class read out of a temporary is retained before the temporary is released", "[ownership]")
{
    // the ordering the node exists to make expressible, and neither half has an arm of its own: the
    // retain is the ordinary "a place is copied" rule, applied by arrive_value while the expression was
    // still the place it was written as - and only then does the temporary close over it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Node { int32 $v; }\n"
        "struct Pair { Node $node; }\n"
        "struct Box {\n"
        "    int32 $seed;\n"
        "    function make() : Pair { return Pair(Node($this->seed)); }\n"
        "}\n"
        "function f(Box& $b) : void { $kept = $b->make()->node; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *bind = binding_in(m);

    REQUIRE(bind != nullptr);
    REQUIRE(bind->body != nullptr);
    REQUIRE(bind->body->get_node_type() == NodeType::n_expr_retain);
    REQUIRE_FALSE(bind->teardown.empty());
}

TEST_CASE("the three positions that cannot hold a temporary refuse one", "[ownership]")
{
    const std::string prelude =
        "struct Buf { ptr<uint8> $data; destructor() { $this->data:$ = null; } }\n"
        "struct Inner { int32 $tag; }\n"
        "struct Outer {\n"
        "    Inner $in;\n"
        "    Buf $buf;\n"
        "    function get() : Inner { return $this->in; }\n"
        "    function owned() : Buf { return Buf(null); }\n"
        "}\n";

    SECTION("a write would be lost")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            prelude + "function f(Outer& $o) : void { $o->get()->tag = 9; }\n");
        REQUIRE(has_issue_containing(*bundle, "would be lost"));
    }

    SECTION("an address kept past the statement would dangle")
    {
        // refused by the *destination*, not by the `&`: an address handed to a callee that reads
        // through it and returns is fine, and is bound around the call instead
        auto bundle = EchoTests::tests_make_parsed_bundle(
            prelude + "function f(Outer& $o) : void { int32& $r = &$o->get()->tag; }\n");
        REQUIRE(has_issue_containing(*bundle, "the address of its member 'tag'"));
    }

    SECTION("but a receiver, which a callee only reads through, is bound around the call")
    {
        // `&$o->get()` written by hand is still refused in the parser - a call is not a place whatever
        // it returns. this is the same address written by the *parser*, for a receiver, and it is safe
        // for a reason the spelling cannot express: the callee reads through it and returns, inside the
        // expression that owns the temporary
        auto bundle = EchoTests::tests_make_parsed_bundle(
            prelude +
            "struct Peek { int32 $n; function twice() : int32 { return $this->n * 2; } }\n"
            "struct Holder { Peek $p; function get() : Peek { return $this->p; } }\n"
            "function f(Holder& $h) : int32 { return $h->get()->twice(); }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        auto *bind = binding_in(m);

        // the temporary wraps the *call*, so it outlives every argument of it - and the `&` the parser
        // wrote for the receiver now addresses an alloca
        REQUIRE(bind != nullptr);
        REQUIRE(bind->body != nullptr);
        REQUIRE(bind->body->get_node_type() == NodeType::n_expr_call);
        REQUIRE(bind->temporaries.size() == 1);
    }

    SECTION("and so would a pointer read out of it")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            prelude + "function f(Outer& $o) : void { $q = $o->owned()->data; }\n");
        REQUIRE(has_issue_containing(*bundle, "the pointer in its member 'data'"));
    }
}

TEST_CASE("a scope control cannot fall out of is owed no drops after its last statement", "[ownership]")
{
    // the scope-exit drops and a `return`'s unwind are two spellings of the same set, so exactly one of
    // them is owed. the pass used to decide by testing its scope's last child for a `ReturnNode`, which is
    // not the question - the question is whether the point past the closing brace is reachable at all, and
    // AST::scope_always_exits is the one thing that answers it
    //
    // none of the duplicates below ever reached the binary: StmtCodegen::gen_scope stops at the first
    // terminated block. they were real nodes in the tree all the same - type-checked, printed by `-ar`
    // where a duplicated drop is meant to be diagnosed, and for a generic local a fresh call site the
    // monomorphizer had to instantiate

    SECTION("a statement written after the return does not bring the set back")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function f() : int32 {\n"
            "    Buffer $b;\n"
            "    return 1;\n"
            "    int32 $dead = 0;\n"
            "}\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        auto &body = body_of(m, "f");

        REQUIRE(drops_on_return(body).size() == 1);
        REQUIRE(drops_in(body).empty());
    }

    SECTION("an `if` whose arms both return leaves nothing to drop after it")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function f(bool $c) : int32 {\n"
            "    Buffer $b;\n"
            "    if ($c) { return 1; } else { return 2; }\n"
            "}\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        auto &body = body_of(m, "f");

        // one on each arm's return, unwinding the enclosing frame the way any early exit does
        auto &branch = body.children.back().get<IfStatementNode>();
        REQUIRE(drops_on_return(*branch.if_scope).size() == 1);
        REQUIRE(drops_on_return(*branch.else_scope).size() == 1);

        // and none appended behind the `if`, where codegen has no block left to put them in
        REQUIRE(drops_in(body).empty());
    }

    SECTION("but an `if` with no else does fall through, and the drop is still owed")
    {
        // the mirror of the case above, and the one a too-eager answer would leak. one arm returning says
        // nothing at all about the other
        auto bundle = EchoTests::tests_make_parsed_bundle(
            std::string(k_buffer) +
            "function f(bool $c) : void {\n"
            "    Buffer $b;\n"
            "    if ($c) { echo 1; }\n"
            "}\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        auto &body = body_of(m, "f");

        auto drops = drops_in(body);
        REQUIRE(drops.size() == 1);
        REQUIRE(dropped_variable(drops[0])->name() == "b");
    }
}
