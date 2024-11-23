#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ExprNode.h>
#include <AST/ScopeNode.h>
#include <AST/VarDeclNode.h>
#include <AST/VarNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;

TEST_CASE("same-named functions stay distinct declarations", "[overloads]")
{
    // the two parse passes used to reconcile on the bare name, so a second `f` adopted the first
    // one's node, cleared its arguments and overwrote its body. one node, the last signature, and
    // the first body leaked
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : int32 { return 1; }\n"
        "function f(int32 $a, int32 $b) : int32 { return 2; }\n"
        "echo f(1);\n"
        "echo f(1, 2);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "f");
    REQUIRE(decls.size() == 2);
    REQUIRE(decls[0] != decls[1]);
    REQUIRE(decls[0]->args.size() == 1);
    REQUIRE(decls[1]->args.size() == 2);
    REQUIRE(decls[0]->body != decls[1]->body);
}

TEST_CASE("each call site resolves to the overload matching its arguments", "[overloads]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : int32 { return 1; }\n"
        "function f(float64 $a) : int32 { return 2; }\n"
        "echo f(1);\n"
        "echo f(1.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "f");
    REQUIRE(calls.size() == 2);

    REQUIRE(calls[0]->decl->args[0]->type().is_integer_type());
    REQUIRE(calls[1]->decl->args[0]->type().is_floating_type());
}

TEST_CASE("an overloaded call still types the variable it initializes", "[overloads]")
{
    // resolution happens during parsing precisely because of this: a variable declared from a
    // call takes its type from the call's return type, and an unresolved call answers void
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : float64 { return 1.5; }\n"
        "function f(float64 $a) : int32 { return 2; }\n"
        "$x = f(1);\n"
        "$y = f(1.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    for (auto *decl : m.nodes.of_type<VarDeclNode>()) {
        if (decl->name() == "$x") {
            REQUIRE(decl->type().is_floating_type());
        }
        if (decl->name() == "$y") {
            REQUIRE(decl->type().is_integer_type());
        }
    }
}

TEST_CASE("an overload declared in another file is callable", "[overloads]")
{
    // every file's symbols are collected before any of them is fully parsed, so the overload set
    // is complete before the first call is resolved
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "echo f(1);\necho f(1.5);\n",
        "function f(int32 $a) : int32 { return 1; }\n"
        "function f(float64 $a) : int32 { return 2; }\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "f");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE(calls[1]->decl != nullptr);
    REQUIRE(calls[0]->decl != calls[1]->decl);
}

TEST_CASE("a struct declared in another file can be constructed", "[overloads]")
{
    // constructors used to be the one exception to that: they were synthesized in the *body* pass,
    // so a `Point(...)` in a file parsed earlier had nothing to resolve against and the program
    // compiled or not depending on the order the files were listed in. they are registered in the
    // declaration pass now, like every other signature
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "$p = Point(1.0, 2.0);\necho $p->x;\n",
        "struct Point { float64 $x; float64 $y; }\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "Point");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE(calls[0]->decl->args.size() == 2);
}

TEST_CASE("a user constructor written in another file still suppresses the field-wise one", "[overloads]")
{
    // the suppression rule asks whether one of *this struct's* constructors already occupies the
    // signature. it used to ask the namespace's overload set for the struct's name, which is being
    // filled as the module is parsed - and which also holds every free function of the same name
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "$p = Point(1.0, 2.0);\n",
        "struct Point {\n"
        "    float64 $x;\n"
        "    float64 $y;\n"
        "    constructor(float64 $a, float64 $b) { $this->x = $a; $this->y = $b; }\n"
        "}\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // one declaration of the name, the user's - not two of the same signature
    auto decls = decls_named(m, "Point");
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0]->args[0]->name() == "a");

    auto calls = calls_to(m, "Point");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl == decls[0]);
}

TEST_CASE("a user constructor of a different signature leaves the field-wise one in place", "[overloads]")
{
    // Echo has no other syntax for building a struct, so a convenience constructor must not take the
    // field-wise one away - both spellings resolve, from a file parsed before the struct's own
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "$a = Point(3.0);\n$b = Point(1.0, 2.0);\n",
        "struct Point {\n"
        "    float64 $x;\n"
        "    float64 $y;\n"
        "    constructor(float64 $v) { $this->x = $v; $this->y = $v; }\n"
        "}\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(decls_named(m, "Point").size() == 2);

    auto calls = calls_to(m, "Point");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl->args.size() == 1);
    REQUIRE(calls[1]->decl->args.size() == 2);
}

TEST_CASE("a struct's own constructor is reachable alongside the field-wise one", "[overloads]")
{
    // the synthesized field-wise constructor is registered under the struct's name, exactly like
    // the user's - they used to collide, and the synthesized one (registered last) always won
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $v) {\n"
        "        $this->x = $v;\n"
        "        $this->y = $v;\n"
        "    }\n"
        "}\n"
        "$a = Point(1);\n"
        "$b = Point(1, 2);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "Point");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl->args.size() == 1);
    REQUIRE(calls[1]->decl->args.size() == 2);
}

TEST_CASE("a constructor body opens with its $this declaration", "[overloads]")
{
    // `$this` has to be the body's *first* child. allocas are emitted in child order, so a `$this`
    // added after the body was parsed had no storage yet when the statements above it wrote
    // through it - and a clone rebinds in child order too, so an instantiated generic would bind
    // its `$this` reads to the template's declaration
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    constructor(int32 $v) { $this->x = $v; }\n"
        "}\n"
        "$p = Point(1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    FunctionDeclNode *ctor = nullptr;
    for (auto *decl : decls_named(m, "Point")) {
        if (decl->args.size() == 1) {
            ctor = decl;
        }
    }

    REQUIRE(ctor != nullptr);
    REQUIRE(ctor->body != nullptr);
    REQUIRE(ctor->body->children.size() > 0);

    auto *first = ctor->body->children[0].get_ptr<VarDeclNode>();
    REQUIRE(first != nullptr);
    REQUIRE(first->name() == "this");
}

TEST_CASE("a generic struct's constructor binds $this to its own instance", "[overloads]")
{
    // the silent half of the same defect: CloneContext::rebind answers with the *original* for
    // anything not yet cloned, so a `$this` declared after the statements that read it left every
    // instance pointing back at the template's declaration
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    constructor(T $v) { $this->value = $v; }\n"
        "}\n"
        "$b = Box<int32>(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    FunctionDeclNode *tmpl = nullptr;
    FunctionDeclNode *instance = nullptr;
    for (auto *decl : decls_named(m, "Box")) {
        if (decl->args.size() != 1 || decl->body == nullptr) {
            continue;
        }
        if (decl->is_generic()) {
            tmpl = decl;
        } else if (decl->is_instantiated()) {
            instance = decl;
        }
    }

    REQUIRE(tmpl != nullptr);
    REQUIRE(instance != nullptr);

    auto *template_this = tmpl->body->children[0].get_ptr<VarDeclNode>();
    auto *instance_this = instance->body->children[0].get_ptr<VarDeclNode>();

    REQUIRE(template_this != nullptr);
    REQUIRE(instance_this != nullptr);
    REQUIRE(template_this->name() == "this");
    REQUIRE(instance_this->name() == "this");

    // the instance owns its own $this. it only can because the declaration is the body's first
    // child: rebind resolves against what has already been cloned, so a $this cloned last would
    // leave every read above it pointing back here, at the template's declaration
    REQUIRE(instance_this != template_this);
    REQUIRE(instance_this->type() == instance->get_return_type());
}

TEST_CASE("two declarations with the same parameter types are rejected", "[overloads]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : int32 { return 1; }\n"
        "function f(int32 $b) : int32 { return 2; }\n"
        "echo f(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "is already declared with these parameter types"));

    // reported once, not once per parse pass
    size_t duplicates = 0;
    for (const auto &issue : bundle->collector.issues) {
        if (issue->message().find("is already declared with these parameter types") != std::string::npos) {
            duplicates++;
        }
    }
    REQUIRE(duplicates == 1);
}

TEST_CASE("an ambiguous call is reported with its candidates", "[overloads]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a, float64 $b) : int32 { return 1; }\n"
        "function f(float64 $a, int32 $b) : int32 { return 2; }\n"
        "echo f(1, 2);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "is ambiguous"));
    REQUIRE(has_issue_containing(*bundle, "f(int32, float64)"));
    REQUIRE(has_issue_containing(*bundle, "f(float64, int32)"));
}

TEST_CASE("a call no overload accepts names the candidates it tried", "[overloads]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : int32 { return 1; }\n"
        "function f(float64 $a) : int32 { return 2; }\n"
        "echo f(1, 2, 3);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "No overload of 'f' accepts these arguments"));
}

TEST_CASE("an undeclared name is still UnknownFunction", "[overloads]")
{
    // a name nobody declared is a different mistake from a name declared differently, and the
    // existing diagnostic for it is unchanged
    auto bundle = EchoTests::tests_make_parsed_bundle("echo nope(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "The function 'nope' could not be found"));
}

TEST_CASE("a concrete overload is preferred over a template that fits equally", "[overloads]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function pick<T>(T $v) : int32 { return 1; }\n"
        "function pick(int32 $v) : int32 { return 2; }\n"
        "echo pick(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "pick");
    REQUIRE(calls.size() == 1);
    REQUIRE_FALSE(calls[0]->decl->is_generic());
    REQUIRE(calls[0]->decl->args[0]->type().is_integer_type());
}

TEST_CASE("a template that fits better than a concrete overload wins", "[overloads]")
{
    // the concrete overload would narrow a float64 to int32; the template matches it exactly.
    // comparing how well the arguments fit has to come before the concrete-beats-template rule
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function pick<T>(T $v) : int32 { return 1; }\n"
        "function pick(int32 $v) : int32 { return 2; }\n"
        "echo pick(1.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "pick");
    REQUIRE(calls.size() == 1);

    // rewired by the monomorphizer to the T=float64 instance
    REQUIRE(calls[0]->decl->is_instantiated());
    REQUIRE(calls[0]->decl->args[0]->type().is_floating_type());
}
