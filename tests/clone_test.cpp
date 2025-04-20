#include <catch2/catch_test_macros.hpp>

#include <AST/ASTClone.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/ASTPlaceExpr.h>
#include <AST/AssignNode.h>
#include <AST/MemberAccessNode.h>
#include <AST/ReturnNode.h>
#include <AST/TemporaryBindExprNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

namespace
{
    FunctionDeclNode *find_func(AST::Module &m, const std::string &name) {
        for (auto *f : m.nodes.of_type<FunctionDeclNode>()) {
            if (!f->is_anonymous() && f->func_name() == name) return f;
        }
        return nullptr;
    }
}

// exercises the node kinds the old hand-rolled cloner silently shared instead of cloning
// (if / while / var-mutation), plus params, locals and binary ops
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
    // so equality here means the whole subtree (if/while/unary/mutation included) cloned)
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

TEST_CASE("Cloning a type declaration shares its layout", "[clone][types]")
{
    // A5. struct equality is ComplexType* pointer identity, and TypeDeclNode holds its layout by value -
    // so a cloned declaration is necessarily a *second* type. The clone used to mint one through
    // ComplexType::substituted_copy, which meant one type had two unequal identities depending on which
    // path a use site came from. TypeRegistry::get_or_create_instantiation is now the only minter, and a
    // type declaration is simply not instantiated: the clone shares it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int32 $x; }\n"
        "P $p = P(41);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    TypeDeclNode *decl = nullptr;
    for (auto *t : module.nodes.of_type<TypeDeclNode>()) {
        if (t->type_name() == "P") decl = t;
    }
    REQUIRE(decl != nullptr);

    TypeSubstitution empty_subst;
    CloneContext cc(module.nodes, empty_subst, bundle->collector.type_registry);

    auto *clone = static_cast<TypeDeclNode *>(decl->clone(cc));

    REQUIRE(clone == decl);
    REQUIRE(clone->value_type() == decl->value_type());
    REQUIRE(clone->value_type().get_complex_type() == decl->value_type().get_complex_type());
}

TEST_CASE("A generic function body carries no second layout into its instances", "[clone][generics]")
{
    // the reachable half of A5: ScopeNode::clone deep-clones every child with no node-type filter, so a
    // `struct` written in a generic body used to be cloned once per instantiation. A body-local type is
    // now refused where a type parameter is visible, so there is nothing in a cloned body to duplicate -
    // this pins that the instances of a plain generic still hold exactly the one declaration the program
    // wrote
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int32 $x; }\n"
        "function outer<T>(T $v) : int32 {\n"
        "    P $p = P(41);\n"
        "    return $p->x;\n"
        "}\n"
        "echo outer<int32>(1);\n"
        "echo outer<float32>(1.0);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    size_t declarations = 0;
    for (auto *t : module.nodes.of_type<TypeDeclNode>()) {
        if (t->type_name() == "P") declarations += 1;
    }

    // two instances of `outer` exist, and neither cloned `P`
    REQUIRE(declarations == 1);
}

TEST_CASE("Cloning a bound temporary rebinds the body onto the clone", "[clone][ownership]")
{
    // the one edge in this node that is not a plain owned child: the body reads out of the temporary
    // through a VarNode, so the declaration has to be cloned *before* the body for cc.rebind to have an
    // answer for it. cloned in the other order the body keeps pointing at the original declaration -
    // which has no alloca in the instance, and the failure surfaces as "Variable has no allocation in
    // scope" from a body nobody wrote (todo/A13b)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Inner { int32 $tag; }\n"
        "struct Outer {\n"
        "    Inner $in;\n"
        "    function get() : Inner { return $this->in; }\n"
        "}\n"
        "function f(Outer& $o) : int32 { return $o->get()->tag; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    TemporaryBindExprNode *bind = nullptr;
    for (auto *node : module.nodes.of_type<TemporaryBindExprNode>()) {
        bind = node;
        break;
    }

    REQUIRE(bind != nullptr);
    REQUIRE(bind->temporaries.size() == 1);

    TypeSubstitution empty_subst;
    CloneContext cc(module.nodes, empty_subst, bundle->collector.type_registry);

    auto *clone = static_cast<TemporaryBindExprNode *>(bind->clone(cc));

    REQUIRE(clone != bind);
    REQUIRE(clone->temporaries.size() == 1);
    REQUIRE(clone->temporaries[0] != bind->temporaries[0]);
    REQUIRE(clone->body != bind->body);

    // the cross-reference followed the declaration rather than staying behind on the original
    auto *access = static_cast<MemberAccessNode *>(clone->body);
    REQUIRE(access->get_node_type() == NodeType::n_member_access);
    REQUIRE(place_root_of(access->get_base_node().unsafe_ptr<ExprNode>()) == clone->temporaries[0]);

    REQUIRE(clone->node_description() == bind->node_description());
}

TEST_CASE("A declaration cloned after a reference to it still rebinds", "[clone]")
{
    // the invariant this asserts is gone: cc.rebind answers with the *original* for anything the map does
    // not hold yet, so a scope cloned in child order alone left every read written *above* a declaration
    // bound to the template's - storage in a function nobody wrote, and silent. ScopeNode::clone clones a
    // scope's declarations before its statements, which is what makes the order below decide nothing
    //
    // the shape is built by reordering rather than written: the parser cannot produce a read above a
    // declaration, and that is exactly why nothing caught this (todo/A12)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function late(int32 $n) : int32 {\n"
        "    int32 $z = $n + 1;\n"
        "    return $z;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    FunctionDeclNode *fn = find_func(module, "late");
    REQUIRE(fn != nullptr);
    REQUIRE(fn->body->children.size() == 2);
    REQUIRE(fn->body->children[0].has_type<VarDeclNode>());

    // move the declaration behind the statement that reads it
    std::swap(fn->body->children[0], fn->body->children[1]);

    REQUIRE(fn->body->children[1].has_type<VarDeclNode>());
    auto *decl = fn->body->children[1].get_ptr<VarDeclNode>();

    TypeSubstitution empty_subst;
    CloneContext cc(module.nodes, empty_subst, bundle->collector.type_registry);

    auto *clone = static_cast<FunctionDeclNode *>(fn->clone(cc));

    REQUIRE(clone->body != fn->body);
    REQUIRE(clone->body->children.size() == 2);
    REQUIRE(clone->body->children[1].has_type<VarDeclNode>());

    auto *cloned_decl = clone->body->children[1].get_ptr<VarDeclNode>();
    REQUIRE(cloned_decl != decl);

    // the read that sits *above* the declaration followed it onto the clone
    REQUIRE(clone->body->children[0].has_type<ReturnNode>());
    auto *ret = clone->body->children[0].get_ptr<ReturnNode>();
    REQUIRE(place_root_of(ret->expr) == cloned_decl);

    // and the declaration was cloned once, not once per reader
    size_t clones_of_decl = 0;
    for (const auto &ref : clone->body->children) {
        if (ref.has_type<VarDeclNode>()) clones_of_decl++;
    }
    REQUIRE(clones_of_decl == 1);
}

TEST_CASE("A synthesized constructor reads $this through a node per use", "[clone]")
{
    // no node may sit in the tree twice. the field-wise constructor used to share one `$this` read
    // between every property write and the `return`, so the node had N+1 parents - and a pass that
    // rewrites a child in place would rewrite it once per parent. it is also what makes cc.child's
    // "already cloned, here is that clone" answer sound: two parents would collapse onto one node
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Pair { int32 $a; int32 $b; }\n"
        "$p = Pair(1, 2);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    FunctionDeclNode *ctor = nullptr;
    for (auto *f : module.nodes.of_type<FunctionDeclNode>()) {
        if (f->member_kind == MemberKind::t_constructor && f->args.size() == 2 && f->body != nullptr) {
            ctor = f;
        }
    }

    REQUIRE(ctor != nullptr);

    // one read of `$this` per property write, plus the `return $this` - every one a node of its own
    std::vector<const Node *> reads;
    for (const auto &child : ctor->body->children) {
        if (child.has_type<AssignNode>()) {
            auto *target = child.get_ptr<AssignNode>()->target;
            reads.push_back(static_cast<MemberAccessNode *>(target)->get_base_node().node());
        }
    }

    REQUIRE(reads.size() == 2);
    REQUIRE(reads[0] != reads[1]);
}
