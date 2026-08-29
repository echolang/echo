#include <catch2/catch_test_macros.hpp>

#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::type_named;

TEST_CASE("constructors() is the user-written set, and a user constructor deletes memberwise", "[constructors][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $v) { $this->x = $v; $this->y = $v; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    REQUIRE(point->constructors().size() == 1);
    REQUIRE(point->synthesized_constructor() == nullptr);
}

TEST_CASE("T(...) through a type parameter constructs after instantiation", "[constructors][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Handle {\n"
        "    int32 $n;\n"
        "    constructor(int32 $n) { $this->n = $n; }\n"
        "}\n"
        "function make<T : class>(int32 $n) : T {\n"
        "    return T($n);\n"
        "}\n"
        "$h = make<Handle>(7);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "T");
    REQUIRE_FALSE(calls.empty());

    // the template body's copy stays pending: T is still a type parameter
    FunctionCallExprNode *in_template = nullptr;
    FunctionCallExprNode *in_instance = nullptr;

    for (auto *call : calls) {
        if (call->constructed_type.is_type_param()) {
            in_template = call;
        } else if (call->constructed_type.has_complex_type() && call->decl != nullptr) {
            in_instance = call;
        }
    }

    REQUIRE(in_template != nullptr);
    REQUIRE(in_template->decl == nullptr);

    REQUIRE(in_instance != nullptr);
    REQUIRE(in_instance->decl->is_constructor());
    REQUIRE_FALSE(in_instance->decl->is_generic());
}

TEST_CASE("T::f(...) through a type parameter resolves after instantiation", "[statics][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Handle {\n"
        "    int32 $n;\n"
        "    constructor(int32 $n) { $this->n = $n; }\n"
        "    static function from(int32 $n) : Handle { return Handle($n); }\n"
        "}\n"
        "function spawn<T : class>(int32 $n) : T {\n"
        "    return T::from($n);\n"
        "}\n"
        "$h = spawn<Handle>(7);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "from");
    REQUIRE_FALSE(calls.empty());

    FunctionCallExprNode *in_template = nullptr;
    FunctionCallExprNode *in_instance = nullptr;

    for (auto *call : calls) {
        if (call->static_owner.is_type_param()) {
            in_template = call;
        } else if (call->static_owner.has_complex_type() && call->decl != nullptr
            && !call->decl->is_generic()) {
            in_instance = call;
        }
    }

    REQUIRE(in_template != nullptr);
    REQUIRE(in_template->decl == nullptr);

    REQUIRE(in_instance != nullptr);
    REQUIRE(in_instance->decl->is_static_method());
}

TEST_CASE("T(...) of a primitive is refused after instantiation", "[constructors][generics]")
{
    EchoTests::assert_code_emits_issue(
        "function build<T>() : T { return T(); }\n"
        "echo build<int32>();\n",
        "The type 'int32' cannot be constructed"
    );
}

TEST_CASE("a constructor cannot declare own type parameters", "[constructors][generics]")
{
    EchoTests::assert_code_emits_issue(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    constructor<A>(A $a) { $this->value = T($a); }\n"
        "}\n",
        "a constructor has no type parameters of its own. the type's are named at the call "
        "('Box<int32>(...)') and a value is built from the arguments"
    );
}
