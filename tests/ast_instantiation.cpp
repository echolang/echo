#include <catch2/catch_test_macros.hpp>

#include <AST/ASTInstantiation.h>
#include <AST/ASTTypeParam.h>
#include <AST/FunctionDeclNode.h>
#include <AST/VarDeclNode.h>

#include <string>
#include <vector>

#include "helpers.h"

// AST::can_instantiate on its own: the one answer to "could a call with these arguments instantiate
// this template", asked the way the two readers ask it
//
// the templates here are declared but never called, so nothing is instantiated behind the test's
// back - what is asserted is the answer, not what a pass then did with it. the *policy* on top of
// each answer is covered where it lives: the diagnostics in tests_eco/errors/, the overload
// filtering in [overloads] and tests_eco/generics/overload_with_template.eco

using namespace AST;

namespace
{
    FunctionDeclNode *template_named(Bundle &bundle, const std::string &name)
    {
        auto &module = bundle.modules.find_module("test");
        auto decls = EchoTests::decls_named(module, name);

        for (auto *decl : decls) {
            if (decl->is_generic()) {
                return decl;
            }
        }

        return nullptr;
    }

    ValueType decl_type(Bundle &bundle, const std::string &varname)
    {
        auto &module = bundle.modules.find_module("test");

        for (auto *decl : module.nodes.of_type<VarDeclNode>()) {
            if (decl->name_full() == varname && decl->has_type()) {
                return decl->type();
            }
        }

        return ValueType::make_unknown();
    }

    const ValueType int32 = EchoTests::prim(ValueTypePrimitive::t_int32);
    const ValueType float64 = EchoTests::prim(ValueTypePrimitive::t_float64);
    const ValueType boolean = EchoTests::prim(ValueTypePrimitive::t_bool);
}

TEST_CASE("Arguments that decide every parameter name an instance", "[instantiation][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function pair<A, B>(A $a, B $b) : A { return $a; }\n");

    auto *tmpl = template_named(*bundle, "pair");
    REQUIRE(tmpl != nullptr);

    const auto inst = can_instantiate(tmpl, {int32, float64});

    REQUIRE(inst.fit == InstantiationFit::t_yes);
    REQUIRE(inst.blame == InstantiationBlame::n_none);
    REQUIRE(inst.decided);

    // declaration order, not the order unification happened to bind in
    REQUIRE(inst.type_arguments == std::vector<ValueType>{int32, float64});
}

TEST_CASE("A different argument count is not a candidate at all", "[instantiation][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function pair<A, B>(A $a, B $b) : A { return $a; }\n");

    const auto inst = can_instantiate(template_named(*bundle, "pair"), {int32});

    REQUIRE(inst.fit == InstantiationFit::t_no);
    REQUIRE(inst.blame == InstantiationBlame::t_argument_count);
    REQUIRE_FALSE(inst.decided);
}

TEST_CASE("A parameter no argument mentions is a cannot-infer", "[instantiation][generics]")
{
    // `U` appears nowhere in the parameter list, so no round will ever bind it. the blame names it,
    // which is how the diagnostic can
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function pick<T, U>(T $x) : T { return $x; }\n");

    const auto inst = can_instantiate(template_named(*bundle, "pick"), {int32});

    REQUIRE(inst.fit == InstantiationFit::t_maybe);
    REQUIRE(inst.blame == InstantiationBlame::t_unbound_parameter);
    REQUIRE(inst.param != nullptr);
    REQUIRE(inst.param->name == "U");
    REQUIRE_FALSE(inst.decided);
}

TEST_CASE("An argument with no type yet binds nothing and is retryable", "[instantiation][generics]")
{
    // the ordinary state of a call inside an un-instantiated template body. the *same* parameter is
    // unbound as in the case above, and the difference is entirely in why - which is the difference
    // between reporting and waiting
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $v) : T { return $v; }\n");

    const auto inst = can_instantiate(template_named(*bundle, "id"), {ValueType::make_unknown()});

    REQUIRE(inst.fit == InstantiationFit::t_maybe);
    REQUIRE(inst.blame == InstantiationBlame::t_undecided_parameter);
    REQUIRE_FALSE(inst.decided);

    // and it did not bind T to unknown on the way past - that would name an instance after
    // information which has not arrived
    REQUIRE(inst.bindings.empty());
}

TEST_CASE("A constraint the binding violates rejects the template and names both sides", "[instantiation][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function only<T: numeric>(T $v) : T { return $v; }\n");

    auto *tmpl = template_named(*bundle, "only");

    const auto allowed = can_instantiate(tmpl, {int32});
    REQUIRE(allowed.fit == InstantiationFit::t_yes);

    const auto rejected = can_instantiate(tmpl, {boolean});

    REQUIRE(rejected.fit == InstantiationFit::t_no);
    REQUIRE(rejected.blame == InstantiationBlame::t_constraint);
    REQUIRE(rejected.param->name == "T");
    REQUIRE(rejected.bound == boolean);

    // the binding exists and is concrete - it is the constraint that says no, which is why the
    // instance is still identified
    REQUIRE(rejected.decided);
}

TEST_CASE("An argument whose shape cannot be reconciled still leaves the instance decided", "[instantiation][generics]")
{
    // the one fact the two orderings disagree about: no substitution makes an int32 fit `Box<T>`, so
    // the template is out of an overload set - but `T` is decided by the other argument, so the
    // instance is well defined and the type checker is the one that reports the argument
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "function unwrap<T>(Box<T> $b, T $fallback) : T { return $fallback; }\n");

    const auto inst = can_instantiate(template_named(*bundle, "unwrap"), {int32, int32});

    REQUIRE(inst.fit == InstantiationFit::t_no);
    REQUIRE(inst.blame == InstantiationBlame::t_argument_shape);
    REQUIRE(inst.argument == 0);

    REQUIRE(inst.decided);
    REQUIRE(inst.type_arguments == std::vector<ValueType>{int32});
}

TEST_CASE("Explicit type arguments win over what the arguments would have inferred", "[instantiation][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $v) : T { return $v; }\n");

    auto *tmpl = template_named(*bundle, "id");

    const auto inst = can_instantiate(tmpl, {int32}, {float64});

    REQUIRE(inst.fit == InstantiationFit::t_yes);
    REQUIRE(inst.decided);
    REQUIRE(inst.type_arguments == std::vector<ValueType>{float64});

    // and they are counted before anything is inferred
    const auto too_many = can_instantiate(tmpl, {int32}, {int32, float64});

    REQUIRE(too_many.fit == InstantiationFit::t_no);
    REQUIRE(too_many.blame == InstantiationBlame::t_type_argument_count);
}

TEST_CASE("A borrow parameter binds through the borrow", "[instantiation][generics][pointer]")
{
    // the address-of a `T&` parameter needs is inserted after inference, so the argument still reads
    // as a value here
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function bump<T>(T &$v) : void { $v = $v + 1; }\n");

    const auto inst = can_instantiate(template_named(*bundle, "bump"), {int32});

    REQUIRE(inst.fit == InstantiationFit::t_yes);
    REQUIRE(inst.type_arguments == std::vector<ValueType>{int32});
}

TEST_CASE("A nullable pointer parameter binds nothing from a value argument", "[instantiation][generics][pointer]")
{
    // ptr<T> does not auto-borrow, so there is no implicit address-of to anticipate
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function hold<T>(ptr<T> $v) : void { }\n");

    const auto inst = can_instantiate(template_named(*bundle, "hold"), {int32});

    // the parameter still mentions `T`, so this is a shape that no substitution reconciles rather
    // than a binding nobody has made yet - out of an overload set, not waiting for a later round
    REQUIRE(inst.fit == InstantiationFit::t_no);
    REQUIRE_FALSE(inst.decided);

    // while the blame is the parameter, because that is the diagnostic a user can act on: `T` was
    // never inferred. the two answers differ on purpose
    REQUIRE(inst.blame == InstantiationBlame::t_unbound_parameter);
}

TEST_CASE("A method's inherited parameters come from the receiver, its own from the call", "[instantiation][generics][methods]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    function widen<U>(U $other) : U { return $other; }\n"
        "}\n"
        "$b = Box(7);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *method = template_named(*bundle, "widen");
    REQUIRE(method != nullptr);
    REQUIRE(method->inherited_type_param_count == 1);

    // the receiver as the parser builds it: a non-nullable borrow of the instance
    const ValueType receiver = ValueType::make_pointer(decl_type(*bundle, "$b"), false);

    const auto inst = can_instantiate(method, {receiver}, {float64});

    REQUIRE(inst.fit == InstantiationFit::t_yes);
    REQUIRE(inst.type_arguments == std::vector<ValueType>{int32, float64});

    // an unresolved receiver is a not-yet, never a "cannot infer": an owner's parameter cannot be
    // spelled at a call site, so there is nothing the user could do about it
    const auto pending = can_instantiate(method, {ValueType::make_unknown()}, {float64});

    REQUIRE(pending.fit == InstantiationFit::t_maybe);
    REQUIRE(pending.blame == InstantiationBlame::t_undecided_parameter);
    REQUIRE(pending.param == method->type_parameters[0]);
}

TEST_CASE("The constraint rule judges everything except a bare type parameter", "[instantiation][generics]")
{
    // shared with the struct-template application in the parser, which is why it takes lists rather
    // than a declaration: `Vec<bool>` and `only_numbers(true)` are one rule with two messages
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function only<T: numeric, U>(T $v, U $w) : T { return $v; }\n");

    auto *tmpl = template_named(*bundle, "only");
    const auto &params = tmpl->type_parameters;

    REQUIRE(first_constraint_violation(params, {int32, boolean}) == std::nullopt);
    REQUIRE(first_constraint_violation(params, {boolean, int32}) == 0);

    // a bare `T` stands for whatever is substituted for it later, so it is judged then
    const ValueType bare = ValueType::make_type_param(params[1]);
    REQUIRE(first_constraint_violation(params, {bare, int32}) == std::nullopt);

    // an unconstrained parameter allows anything, and a missing argument is not judged at all
    REQUIRE(first_constraint_violation(params, {int32}) == std::nullopt);
}
