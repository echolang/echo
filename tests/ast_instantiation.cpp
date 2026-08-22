#include <catch2/catch_test_macros.hpp>

#include <AST/ASTInstantiation.h>
#include <AST/ASTTypeParam.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
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

TEST_CASE("A prefix of explicit type arguments binds and the rest is inferred", "[instantiation][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function make<T, A>(A $arg) : T { return T($arg); }\n"
        "class Handle { int32 $n; constructor(int32 $n) { $this->n = $n; } }\n");

    auto *tmpl = template_named(*bundle, "make");
    REQUIRE(tmpl != nullptr);

    auto &m = bundle->modules.find_module("test");
    auto *handle = EchoTests::type_named(m, "Handle");
    REQUIRE(handle != nullptr);

    const ValueType handle_ty = handle->value_type();
    const auto inst = can_instantiate(tmpl, {int32}, {handle_ty});

    REQUIRE(inst.fit == InstantiationFit::t_yes);
    REQUIRE(inst.decided);
    REQUIRE(inst.type_arguments.size() == 2);
    REQUIRE(inst.type_arguments[0] == handle_ty);
    REQUIRE(inst.type_arguments[1] == int32);
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

TEST_CASE("A class-kind constraint admits classes and refuses everything else", "[instantiation][generics]")
{
    // `T : class` is an open kind predicate, not a closed alias. a generic class instantiation
    // carries the template's kind, so Box<int32> answers yes the same way a plain Handle does
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Handle { int32 $n; }\n"
        "struct Point { int32 $x; }\n"
        "class Box<U> { U $v; }\n"
        "function only<T: class>(T $v) : T { return $v; }\n"
        "Handle $h = Handle(1);\n"
        "Point $p = Point(1);\n"
        "Box<int32> $b = Box<int32>(1);\n");

    auto *tmpl = template_named(*bundle, "only");
    REQUIRE(tmpl != nullptr);
    REQUIRE(tmpl->type_parameters[0]->constraint_spelling == "class");
    REQUIRE(tmpl->type_parameters[0]->constraint.size() == 1);
    REQUIRE(tmpl->type_parameters[0]->constraint[0].is_class_kind_constraint());

    const ValueType handle = decl_type(*bundle, "$h");
    const ValueType point = decl_type(*bundle, "$p");
    const ValueType boxed = decl_type(*bundle, "$b");

    REQUIRE(handle.is_class());
    REQUIRE(point.is_struct());
    REQUIRE(boxed.is_class());

    REQUIRE(first_constraint_violation(tmpl->type_parameters, {handle}) == std::nullopt);
    REQUIRE(first_constraint_violation(tmpl->type_parameters, {boxed}) == std::nullopt);
    REQUIRE(first_constraint_violation(tmpl->type_parameters, {point}) == 0);
    REQUIRE(first_constraint_violation(tmpl->type_parameters, {int32}) == 0);
}

TEST_CASE("a nullable generic application stays nullable after substitution", "[instantiation][generics]")
{
    // `Box<T>?` is a nullable Box, not Box<T?>. substitute_type's instantiation arm rebuilt
    // the application and copied const, not nullability, so `$this->x` in the instance was
    // `Box<int32>` and a guard over it was refused
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Box<T> { T $v; constructor(T $v) { $this->v = $v; } }\n"
        "struct Hold<T> { Box<T>? $x; }\n"
        "Hold<int32> $h = Hold<int32>(null);\n"
        "function take(Hold<int32> $from) : int32 {\n"
        "    Box<int32> $b = guard $from->x else { return 0; }\n"
        "    return $b->v;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    const ValueType hold = decl_type(*bundle, "$h");
    REQUIRE(hold.has_complex_type());
    const ComplexType *ct = hold.get_complex_type();
    REQUIRE(ct->property_count() == 1);
    const ValueType &slot = ct->get_property_type(0);
    REQUIRE(slot.is_class());
    REQUIRE(slot.is_nullable());
}

TEST_CASE("a nullable C function pointer stays nullable after substitution", "[instantiation][generics][cfn]")
{
    // the signature arm rebuilt `extern function<T(T)>` and copied const, not `?`. a property
    // of that type on a generic struct became non-nullable in every instance
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id(int32 $x) : int32 { return $x; }\n"
        "struct Hold<T> { extern function<T(T)>? $op; }\n"
        "Hold<int32> $h = Hold<int32>(null);\n"
        "function take(Hold<int32> $from) : int32 {\n"
        "    extern function<int32(int32)> $f = guard $from->op else { return -1; }\n"
        "    return $f(1);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    const ValueType hold = decl_type(*bundle, "$h");
    REQUIRE(hold.has_complex_type());
    const ComplexType *ct = hold.get_complex_type();
    REQUIRE(ct->property_count() == 1);
    const ValueType &slot = ct->get_property_type(0);
    REQUIRE(slot.is_c_function());
    REQUIRE(slot.is_nullable());
}
