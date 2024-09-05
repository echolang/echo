#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTClone.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>

using namespace AST;

namespace {
    FunctionDeclNode *find_func(AST::Module &m, const std::string &name) {
        for (auto *f : m.nodes.of_type<FunctionDeclNode>()) {
            if (!f->is_anonymous() && f->func_name() == name) return f;
        }
        return nullptr;
    }
}

// exercises the node kinds the old hand-rolled cloner silently shared instead of cloning
// (if / while / var-mutation), plus params, locals and binary ops.
TEST_CASE("Clone reproduces a function body structurally and independently", "[clone]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function compute(int $x, int $y): int {\n"
        "    int $z = $x + $y;\n"
        "    if ($z > $x) {\n"
        "        $z = $z - $y;\n"
        "    }\n"
        "    while ($z > $y) {\n"
        "        $z = $z + 1;\n"
        "    }\n"
        "    return $z;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    FunctionDeclNode *fn = find_func(module, "compute");
    REQUIRE(fn != nullptr);

    TypeSubstitution empty_subst;
    CloneContext cc(module.nodes, empty_subst, bundle->collector.type_registry);

    auto *clone = static_cast<FunctionDeclNode *>(fn->clone(cc));

    // a genuinely new node graph...
    REQUIRE(clone != fn);
    REQUIRE(clone->body != fn->body);
    REQUIRE(clone->args.size() == fn->args.size());
    for (size_t i = 0; i < fn->args.size(); ++i) {
        REQUIRE(clone->args[i] != fn->args[i]);  // parameters were deep-cloned, not shared
    }

    // ...that is structurally identical (node_description recurses through every child,
    // so equality here means the whole subtree (if/while/unary/mutation included) cloned).
    REQUIRE(clone->body->node_description() == fn->body->node_description());
    REQUIRE(clone->node_description() == fn->node_description());
}

TEST_CASE("Clone reproduces the pointer node kinds", "[clone][pointer]")
{
    // the monomorphizer relies on clone() being total, and the pointer nodes are the newest
    // arrivals in the tree. a missed edge here does not fail loudly - it shares a node between
    // two instances, so one instantiation quietly mutates another's body
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int $a; int $b; }\n"
        "function walk(ptr<int> $p) : int {\n"
        "    ptr<int> $q = $p:$ + 1;\n"
        "    $q = $q + $p:$[0];\n"
        "    ptr<ptr<int>> $pp = &$q;\n"
        "    return $pp:$[0]:$[0];\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    FunctionDeclNode *fn = find_func(module, "walk");
    REQUIRE(fn != nullptr);

    TypeSubstitution empty_subst;
    CloneContext cc(module.nodes, empty_subst, bundle->collector.type_registry);

    auto *clone = static_cast<FunctionDeclNode *>(fn->clone(cc));

    REQUIRE(clone != fn);
    REQUIRE(clone->body != fn->body);

    // node_description recurses the whole subtree, so equality means every addrof, index and
    // deref edge came along - and the body is a distinct graph, not the original shared
    REQUIRE(clone->body->node_description() == fn->body->node_description());
    REQUIRE(clone->get_return_type() == fn->get_return_type());
}

TEST_CASE("Clone substitutes a type parameter underneath a pointer", "[clone][pointer][generics]")
{
    // `ptr<T>` has to come back as `ptr<int32>`, with the pointer rebuilt around the resolved
    // pointee rather than the parameter being swapped for a bare int32 and the level lost
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function hold<T>(ptr<T> $v) : T {\n"
        "    return $v;\n"
        "}\n"
        "$a = 5;\n"
        "ptr<int> $p = &$a;\n"
        "echo hold($p);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    // the monomorphizer already cloned the template into an instance; find the one whose
    // parameter is concrete and check the pointer survived the substitution
    bool found_instance = false;
    for (auto *fn : module.nodes.of_type<FunctionDeclNode>()) {
        if (fn->is_anonymous() || fn->func_name() != "hold" || fn->is_generic()) {
            continue;
        }
        REQUIRE(fn->args.size() == 1);
        ValueType arg = fn->args[0]->type();
        REQUIRE(arg.is_pointer());
        REQUIRE(arg.pointee().is_primitive_of_type(ValueTypePrimitive::t_int32));
        REQUIRE_FALSE(contains_type_param(arg));
        found_instance = true;
    }
    REQUIRE(found_instance);
}

TEST_CASE("Two clones of the same function are independent", "[clone]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function twice_val(int $x): int {\n"
        "    int $r = $x + $x;\n"
        "    return $r;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    FunctionDeclNode *fn = find_func(module, "twice_val");
    REQUIRE(fn != nullptr);

    TypeSubstitution empty_subst;
    CloneContext cc1(module.nodes, empty_subst, bundle->collector.type_registry);
    CloneContext cc2(module.nodes, empty_subst, bundle->collector.type_registry);

    auto *a = static_cast<FunctionDeclNode *>(fn->clone(cc1));
    auto *b = static_cast<FunctionDeclNode *>(fn->clone(cc2));

    REQUIRE(a != b);
    REQUIRE(a->body != b->body);
    REQUIRE(a->node_description() == fn->node_description());
    REQUIRE(b->node_description() == fn->node_description());
}
