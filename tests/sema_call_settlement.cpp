#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::has_issue_containing;

// resolution is *attempted* where a call is written, because the call's type is needed there -
// `$x = f(1);` takes the variable's type from it. but an argument's type is not necessarily final at
// that moment, and a decision made against a type that says nothing is a wrong decision rather than
// a missing one. so a call carries how far it has been taken, and the monomorphizer's fixpoint - the
// same one that answers those types - finishes it
//
// the invariant these pin: a local behaves identically at an argument position whether its type was
// written out or inferred from a generic call

namespace
{
    // the node an argument ultimately is, for asserting the *shape* the coercion produced rather
    // than only that the program compiled
    NodeType argument_kind(Module &m, const std::string &call_name, size_t index)
    {
        auto found = calls_to(m, call_name);
        REQUIRE(found.size() >= 1);
        REQUIRE(found[0]->arguments.size() > index);
        return found[0]->arguments[index]->get_node_type();
    }
}

TEST_CASE("a local from a generic constructor auto-borrows like any other", "[call_settlement]")
{
    // the defect: at parse time `$q` still had the template's `Q<T>`, which argument_fit reads as
    // undetermined - so the implicit borrow declined to wrap and the fallback beneath it committed a
    // cast for a mismatch that was never one. the monomorphizer then answered `$q : Q<int32>` and the
    // type checker reported the parser's own cast
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Q<T> { T $x; }\n"
        "function f(Q<int32>& $s) : int32 { return $s->x; }\n"
        "$q = Q<int32>(2);\n"
        "echo f($q);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // an address, not a cast. this is the whole acceptance criterion
    REQUIRE(argument_kind(m, "f", 0) == NodeType::n_expr_addrof);

    auto found = calls_to(m, "f");
    REQUIRE(found[0]->settlement == CallSettlement::t_settled);
    REQUIRE(found[0]->decl != nullptr);
}

TEST_CASE("an inferred and a written local reach the same tree", "[call_settlement]")
{
    // stated as the invariant rather than as the one reported spelling: the two declarations differ
    // only in who worked the type out, so every argument position has to be indistinguishable
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Q<T> { T $x; }\n"
        "function f(Q<int32>& $s) : int32 { return $s->x; }\n"
        "Q<int32> $written = Q<int32>(1);\n"
        "$inferred = Q<int32>(2);\n"
        "echo f($written);\n"
        "echo f($inferred);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto found = calls_to(m, "f");
    REQUIRE(found.size() == 2);

    REQUIRE(found[0]->decl == found[1]->decl);
    REQUIRE(found[0]->arguments[0]->get_node_type() == found[1]->arguments[0]->get_node_type());
    REQUIRE(found[0]->arguments[0]->result_type() == found[1]->arguments[0]->result_type());
    REQUIRE(found[0]->settlement == CallSettlement::t_settled);
    REQUIRE(found[1]->settlement == CallSettlement::t_settled);
}

TEST_CASE("a by-value parameter takes a generic-constructed local unwrapped", "[call_settlement]")
{
    // the other argument position: nothing to wrap and nothing to cast, so the argument must arrive
    // as written. the old coercion loop inserted a cast here too, because `Q<T>` and `Q<int32>` are
    // not implicitly convertible and it could not tell that from "no information yet"
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Q<T> { T $x; }\n"
        "function byvalue(Q<int32> $s) : int32 { return $s->x; }\n"
        "$q = Q<int32>(5);\n"
        "echo byvalue($q);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(argument_kind(m, "byvalue", 0) == NodeType::n_varref);
}

TEST_CASE("an overload set undecidable at parse time resolves in the fixpoint", "[call_settlement]")
{
    // both candidates score neutrally against an undetermined argument, so nothing separates them and
    // the matcher answers t_undecidable. that was reported as an error on the spot; it is a not-yet
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Q<T> { T $x; }\n"
        "struct P { int32 $y; }\n"
        "function pick(Q<int32>& $q) : int32 { return $q->x; }\n"
        "function pick(P& $p) : int32 { return $p->y; }\n"
        "$q = Q<int32>(7);\n"
        "$p = P(9);\n"
        "echo pick($q);\n"
        "echo pick($p);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto found = calls_to(m, "pick");
    REQUIRE(found.size() == 2);

    // each resolved, and to a *different* declaration - the point of deferring rather than guessing
    REQUIRE(found[0]->decl != nullptr);
    REQUIRE(found[1]->decl != nullptr);
    REQUIRE(found[0]->decl != found[1]->decl);
    REQUIRE(found[0]->settlement == CallSettlement::t_settled);
    REQUIRE(found[1]->settlement == CallSettlement::t_settled);
}

TEST_CASE("a call that never becomes decidable is still reported", "[call_settlement]")
{
    // `null` fits both, and no round is going to change that. reported by the finalizing sweep once
    // the fixpoint is out of rounds, which is what proves nothing was coming - and reported *once*,
    // because collect_issue de-duplicates on kind, token range and message
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(ptr<int32> $p) : int32 { return 1; }\n"
        "function f(ptr<float64> $p) : int32 { return 2; }\n"
        "echo f(null);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot be resolved"));

    size_t reports = 0;
    for (const auto &issue : bundle->collector.issues) {
        if (issue->message().find("cannot be resolved") != std::string::npos) {
            reports++;
        }
    }
    REQUIRE(reports == 1);
}

TEST_CASE("no call reaches the end of the pipeline unsettled", "[call_settlement]")
{
    // codegen dereferences `decl`, and a pending call is only safe because the sweep at the end of
    // the fixpoint either resolves it or reports it. assert the invariant over a program that mixes
    // every shape: generic and concrete callees, a method, a drop the ownership pass inserted
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Q<T> {\n"
        "    T $x;\n"
        "    function get() : T { return $this->x; }\n"
        "}\n"
        "function twice<T>(T $v) : T { return $v; }\n"
        "function f(Q<int32>& $s) : int32 { return $s->x; }\n"
        "$q = Q<int32>(2);\n"
        "echo f($q);\n"
        "echo $q->get();\n"
        "echo twice<int32>(3);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    for (auto *call : m.nodes.of_type<FunctionCallExprNode>()) {
        // a call still naming a template is one inside a template nobody instantiated - its body is
        // never emitted, so it is the one shape allowed to stay unsettled
        if (call->decl != nullptr && call->decl->is_generic()) {
            continue;
        }

        REQUIRE(call->settlement == CallSettlement::t_settled);

        // `echo` is the one settled call with no declaration: it borrows the node's shape without
        // being a call, and codegen lowers it from its name token into a printf. everything else
        // names something, or the finalizing sweep would have reported it
        if (call->token_function_name.value() == "echo") {
            REQUIRE(call->decl == nullptr);
            continue;
        }

        REQUIRE(call->decl != nullptr);
    }
}
