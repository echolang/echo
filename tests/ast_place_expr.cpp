#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/ASTPlaceExpr.h>
#include <AST/ExprNode.h>
#include <AST/LiteralValueNode.h>
#include <AST/MemberAccessNode.h>
#include <AST/NullNode.h>
#include <AST/TemporaryBindExprNode.h>
#include <AST/VarDeclNode.h>
#include <AST/VarNode.h>
#include <AST/VarRefNode.h>

#include "helpers.h"

// AST::is_place_expression - "does this expression denote storage".
//
// four consumers have to agree on the answer: the parser rejecting `&($a + $b)`, the adjustment
// pass deciding value versus place position, the type checker locating a diagnostic, and the
// lvalue codegen's dispatch. when each kept its own switch they drifted, and member reads ended
// up disagreeing with member writes (todo/B4). pinning the exact tag set here makes widening it a
// deliberate act rather than a side effect

using namespace AST;

namespace
{
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

    // the *container* index in a module, as opposed to the pointer one inside the element operator's
    // own body - `return &$b->at:$[$i]` is an index expression too, and it comes first
    IndexExprNode *first_rewritten_index(Bundle &bundle)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *node : module.nodes.of_type<IndexExprNode>()) {
            if (node->element_call != nullptr) {
                return node;
            }
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

TEST_CASE("can_bind_temporary admits a value with no home, and only that", "[AST][pointer]")
{
    // the expression half of todo/A13c: a non-place may answer a borrow parameter, because a slot can be
    // *minted* for it. so this and is_place_expression partition the arguments a borrow accepts, and the
    // exclusions below are what keep the partition from swallowing rules that belong elsewhere
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function get() : int { return 1; }\n"
        "$a = 5;\n"
        "ptr<int> $p = &$a;\n"
        "$sum = $a + 1;\n"
        "echo get();\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *literal = first_of<LiteralIntExprNode>(*bundle);
    auto *call = first_of<FunctionCallExprNode>(*bundle);
    auto *binary = first_of<BinaryExprNode>(*bundle);
    auto *addr = first_of<AddrOfExprNode>(*bundle);
    auto *var_ref = first_of<VarRefNode>(*bundle);

    REQUIRE(literal != nullptr);
    REQUIRE(call != nullptr);
    REQUIRE(binary != nullptr);
    REQUIRE(addr != nullptr);
    REQUIRE(var_ref != nullptr);

    // a value the program computed and did not name
    REQUIRE(can_bind_temporary(*literal));
    REQUIRE(can_bind_temporary(*call));
    REQUIRE(can_bind_temporary(*binary));

    // **an address is already a value that means an address.** minting storage for one hands out a
    // ptr<ptr<T>>, which AST::OwnershipPass refuses anyway - so it is excluded here, where the refusal
    // costs a rank rather than a diagnostic nobody can act on
    REQUIRE_FALSE(can_bind_temporary(*addr));

    // and a place needs nothing minted: it reaches t_borrow and never sees the temporary arm at all
    REQUIRE_FALSE(can_bind_temporary(*var_ref));
}

TEST_CASE("An assignable target is every place, plus a peel", "[AST][pointer]")
{
    // is_assignable_target is deliberately one wider than is_place_expression: `$p:$ = &$b`
    // re-seats the pointer, so the peel is a legal destination even though it has no address of
    // its own. an address (`$p:$:$`) is not - there is nothing behind it to write into
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int $a; }\n"
        "function get() : int { return 1; }\n"
        "$s = P(1);\n"
        "ptr<int> $p = &$s->a;\n"
        "echo $p:$[1];\n"
        "echo $p;\n"
        "echo ($p:$ == null);\n"
        "echo get();\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *var_ref = first_of<VarRefNode>(*bundle);
    auto *member = first_of<MemberAccessNode>(*bundle);
    auto *index = first_of<IndexExprNode>(*bundle);
    auto *deref = first_of<DerefExprNode>(*bundle);
    auto *peel = first_of<PointerValueNode>(*bundle);
    auto *addr = first_of<AddrOfExprNode>(*bundle);
    auto *literal = first_of<LiteralIntExprNode>(*bundle);
    auto *call = first_of<FunctionCallExprNode>(*bundle);

    REQUIRE(peel != nullptr);
    REQUIRE(addr != nullptr);
    REQUIRE(literal != nullptr);
    REQUIRE(call != nullptr);

    REQUIRE(is_assignable_target(*var_ref));
    REQUIRE(is_assignable_target(*member));
    REQUIRE(is_assignable_target(*index));
    REQUIRE(is_assignable_target(*deref));
    REQUIRE(is_assignable_target(*peel));

    REQUIRE_FALSE(is_assignable_target(*addr));
    REQUIRE_FALSE(is_assignable_target(*literal));
    REQUIRE_FALSE(is_assignable_target(*call));
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
    // NullNode is an ExprNode, but `n_null` is intentionally absent from is_expression_node()
    //
    // that predicate gates try_implicit_cast (src/Parser/ExprParser.cpp), and null must not be
    // wrapped in a TypeCastNode: it has no type of its own, it acquires one through bound_type,
    // and the type checker's null-specific rules all test for the raw n_null tag. wrapping it
    // would hide the tag and turn "cannot be null" into a silent conversion
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

// **a rewritten index is still a place**, and that is the whole reason indexing stays one node.
// AST::is_place_expression answers on the *tag*, so `$a[$i]` reads, writes, takes `&` and chains
// with `->` whether the base turned out to be a pointer or a container - a separate "container
// index" node would have had to re-derive every one of those (the mistake B16 records)
TEST_CASE("A container index is a place, before and after the rewrite", "[AST][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; }\n"
        "operator (Bag& $b)[usize $i] : int32& { return &$b->at:$[$i]; }\n"
        "int32 $x = 1;\n"
        "$g = Bag(&$x);\n"
        "$g[0] = 5;\n"
        "echo $g[0];\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *index = first_rewritten_index(*bundle);
    REQUIRE(index != nullptr);

    // the rewriter ran, so the operands moved into the call and `base` was cleared - one owner per
    // edge, because PointerAdjuster rewrites edges in place and a shared one is adjusted twice
    REQUIRE(index->element_call != nullptr);
    REQUIRE(index->base == nullptr);
    REQUIRE(index->indices.empty());

    // ...and none of that changes what it *is*
    REQUIRE(is_place_expression(*index));
    REQUIRE(is_assignable_target(*index));

    // the element, not the container and not the borrow the operator hands back
    REQUIRE(index->result_type().is_primitive_of_type(ValueTypePrimitive::t_int32));
}

// the honest answer while the contract has not been attached yet. peeling the base there would hand
// back the *container* as though it were the element, and a confidently wrong type is one no later
// pass could tell from a right one - so an inferred declaration would latch onto it
TEST_CASE("An unresolved container index answers unknown, not the container", "[AST][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; }\n"
        "int32 $x = 1;\n"
        "$g = Bag(&$x);\n"
        "echo $g[0];\n");

    // no element contract, so the rewriter reports rather than attaching one
    REQUIRE(bundle->collector.has_critical_issues());

    auto *index = first_of<IndexExprNode>(*bundle);
    REQUIRE(index != nullptr);
    REQUIRE(index->element_call == nullptr);
    REQUIRE(index->result_type().is_unknown());
}

// **the two states of the node, under `clone`.** the monomorphizer clones a template body per
// instantiation, and exactly one of the two states owns the operands - before the rewrite they hang
// off the node, after it they are the call's arguments and `base` is null. cloning both would
// duplicate the subtree under two parents, which AST::PointerAdjuster then rewrites twice
//
// no end-to-end case can see this: the second deref it would insert produces a wrong *value*, not a
// diagnostic, and only for a generic container instantiated more than once
TEST_CASE("A cloned index carries exactly one owner of its operands", "[AST][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag<T> { ptr<T> $at; }\n"
        "operator<T> (Bag<T>& $b)[usize $i] : T& { return &$b->at:$[$i]; }\n"
        "function first<T>(Bag<T>& $b) : T { return $b[0]; }\n"
        "int32 $x = 1;\n"
        "float64 $y = 2.0;\n"
        "$bi = Bag<int32>(&$x);\n"
        "$bf = Bag<float64>(&$y);\n"
        "echo first<int32>(&$bi);\n"
        "echo first<float64>(&$bf);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    size_t rewritten = 0;

    for (auto *node : module.nodes.of_type<IndexExprNode>()) {
        // every node is in one state or the other, never both and never neither
        const bool is_container = node->element_call != nullptr;

        if (is_container) {
            rewritten++;
            REQUIRE(node->base == nullptr);
            REQUIRE(node->indices.empty());
        } else {
            REQUIRE(node->base != nullptr);
        }
    }

    // `first<T>`'s body was cloned twice, and the template's own copy stays undecided - so the
    // instances are what carry the rewritten nodes
    REQUIRE(rewritten >= 2);
}

// the one answer to "is this a pointer index", read by the rewriter *and* by result_type(). it peels
// **non-nullable** levels and stops at a nullable one, which is AST::argument_fit's t_read_through
// line: reading through a `ptr<T>` that may be null is an unchecked dereference. that is the whole
// of what makes `$a[0]` over an `Array<T>&` parameter index the array while `$p[0]` stays a pointer
TEST_CASE("indexed_base_type peels borrows but stops at a nullable pointer", "[AST][pointer]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; }\n"
        "operator (Bag& $b)[usize $i] : int32& { return &$b->at:$[$i]; }\n"
        "function through(Bag& $b) : int32 { return $b[0]; }\n"
        "int32 $x = 1;\n"
        "$g = Bag(&$x);\n"
        "echo through(&$g);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // `$b[0]` inside `through` indexes through a `Bag&` parameter, so the borrow is peeled and the
    // container is what answers - it resolved, which is the observable form of that
    auto *rewritten = first_rewritten_index(*bundle);
    REQUIRE(rewritten != nullptr);

    // the pointer index inside the operator's own body kept its base: `$b->at:$` is a `ptr<int32>`,
    // nullable, so nothing is peeled and the GEP arm is what lowers it
    auto &module = bundle->modules.find_module("test");
    bool saw_pointer_index = false;

    for (auto *node : module.nodes.of_type<IndexExprNode>()) {
        if (node->element_call == nullptr && node->base != nullptr) {
            REQUIRE(node->indexed_base_type().is_pointer());
            REQUIRE(node->base_was_peeled);
            saw_pointer_index = true;
        }
    }

    REQUIRE(saw_pointer_index);
}

TEST_CASE("A bound temporary is not a place either", "[AST][pointer][ownership]")
{
    // its value is a copy read out of something about to be destroyed, so `&$o->get()->x` would hand
    // out an address that dangles at the end of the statement and `$o->get()->x = 5` would write into
    // bytes nothing will ever read. both are refused in AST::OwnershipPass, which is the only pass that
    // knew a temporary was wanted - and this predicate staying narrow is what keeps them refused
    // everywhere else too (todo/A13b)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Inner { int32 $tag; }\n"
        "struct Outer {\n"
        "    Inner $in;\n"
        "    function get() : Inner { return $this->in; }\n"
        "}\n"
        "function f(Outer& $o) : int32 { return $o->get()->tag; }\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *bind = first_of<TemporaryBindExprNode>(*bundle);

    REQUIRE(bind != nullptr);
    REQUIRE_FALSE(is_place_expression(*bind));
    REQUIRE_FALSE(is_assignable_target(*bind));

    // and nothing under it names a variable, which is what AST::TypeChecker's "cannot return the
    // address of a local" reads - the reason a pointer read out of a temporary has to be refused where
    // the temporary is created rather than left to that check
    REQUIRE(place_root_of(bind) == nullptr);
}
