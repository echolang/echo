#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConformance.h>
#include <AST/ASTInstantiation.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ASTTypeParam.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::prim;
using EchoTests::type_named;

namespace
{
    // the interface's requirement mentions the interface's own `T`, and the implementor's method
    // mentions the implementor's own `E`. that is the case the substitution exists for: they are
    // different TypeParamDecls, and only binding T through the conformance makes them comparable
    const char *k_generic =
        "interface Sized<T> {\n"
        "    function first() : T;\n"
        "}\n"
        "struct Bag<E> : Sized<E> {\n"
        "    E $item;\n"
        "    function first() : E { return $this->item; }\n"
        "}\n";

    // an instantiation of `Bag`, so the registry actually interns one for the assertions below
    const char *k_generic_used =
        "interface Sized<T> {\n"
        "    function first() : T;\n"
        "}\n"
        "struct Bag<E> : Sized<E> {\n"
        "    E $item;\n"
        "    function first() : E { return $this->item; }\n"
        "}\n"
        "$b = Bag<int32>(7);\n"
        "echo $b->first();\n";
}

// **the substitution is the whole of this feature's generic story.** an instantiation's conformances
// are derived when it is interned, not redirected to the template at read time - so `Bag<int32>` says
// it conforms to `Sized<int32>`, which is a type a use site can actually name. redirect instead and the
// answer is `Sized<E>`, which equals nothing and silently satisfies no constraint
TEST_CASE("an instantiation's conformance is substituted, not the template's", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_generic_used);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *bag = type_named(m, "Bag");
    auto *sized = type_named(m, "Sized");
    REQUIRE(bag != nullptr);
    REQUIRE(sized != nullptr);

    ComplexType &bag_template = bag->complex_type();
    ComplexType &sized_template = sized->complex_type();
    REQUIRE(bag_template.is_generic());

    // the template conforms to `Sized<E>` - its own parameter, unbound
    REQUIRE(bag_template.conformances().size() == 1);
    REQUIRE(AST::contains_type_param(bag_template.conformances()[0]));

    // and the instantiation conforms to `Sized<int32>`, which mentions no parameter at all
    auto *bag_int = bundle->collector.type_registry.get_or_create_instantiation(
        &bag_template, { prim(ValueTypePrimitive::t_int32) });
    auto *sized_int = bundle->collector.type_registry.get_or_create_instantiation(
        &sized_template, { prim(ValueTypePrimitive::t_int32) });

    REQUIRE(bag_int->conformances().size() == 1);
    REQUIRE_FALSE(AST::contains_type_param(bag_int->conformances()[0]));
    REQUIRE(bag_int->conformances()[0] == ValueType::make_complex(sized_int));

    REQUIRE(AST::conforms_to(bag_int, ValueType::make_complex(sized_int)));

    // ...and not to the template's application, nor to a different one
    REQUIRE_FALSE(AST::conforms_to(bag_int, sized->value_type()));

    auto *sized_f64 = bundle->collector.type_registry.get_or_create_instantiation(
        &sized_template, { prim(ValueTypePrimitive::t_float64) });
    REQUIRE_FALSE(AST::conforms_to(bag_int, ValueType::make_complex(sized_f64)));
}

// conforms_to is a *claim* test and answers for anything, settled or not - every reader asks it about
// types that may still be in flight. so the shapes that carry no conformance answer false rather than
// asserting or reaching through a null layout
TEST_CASE("conforms_to is total", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_generic);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *sized = type_named(m, "Sized");
    REQUIRE(sized != nullptr);
    const ValueType iface = sized->value_type();

    REQUIRE_FALSE(AST::conforms_to(nullptr, iface));
    REQUIRE_FALSE(AST::conforms_to(prim(ValueTypePrimitive::t_int32), iface));
    REQUIRE_FALSE(AST::conforms_to(ValueType::make_unknown(), iface));
    REQUIRE_FALSE(AST::conforms_to(ValueType::make_pointer(prim(ValueTypePrimitive::t_int32), true), iface));
    REQUIRE_FALSE(AST::conforms_to(ValueType::make_callable(ValueType::void_type(), {}), iface));

    // and a right-hand side that is not an interface is never conformed to, whatever the left is
    auto *bag = type_named(m, "Bag");
    REQUIRE(bag != nullptr);
    REQUIRE_FALSE(AST::conforms_to(&bag->complex_type(), bag->value_type()));
    REQUIRE_FALSE(AST::conforms_to(&bag->complex_type(), prim(ValueTypePrimitive::t_int32)));
}

// the receiver is `Drawable&` on the requirement and `Square&` on the implementor **by construction** -
// a method's `$this` is its owner's borrow. so the comparison starts at parameter 1, and this is the
// test that fails if somebody ever compares from 0: every conformance in the language would break
TEST_CASE("a requirement is satisfied ignoring the receiver but nothing else", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Shifter {\n"
        "    function shift(int32 $by, bool $wrap) : int32;\n"
        "}\n"
        "struct Reg : Shifter {\n"
        "    int32 $bits;\n"
        "    function shift(int32 $by, bool $wrap) : int32 { return $by; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *reg = type_named(m, "Reg");
    auto *shifter = type_named(m, "Shifter");
    REQUIRE(reg != nullptr);
    REQUIRE(shifter != nullptr);

    // the two receivers genuinely differ, which is what makes the skip load-bearing rather than cosmetic
    auto requirements = AST::interface_requirements(&shifter->complex_type());
    REQUIRE(requirements.size() == 1);
    auto own = AST::find_member_functions(&reg->complex_type(), "shift");
    REQUIRE(own.size() == 1);
    REQUIRE_FALSE(requirements[0]->parameter_type(0) == own[0]->parameter_type(0));

    REQUIRE_FALSE(AST::first_unmet_requirement(
        &reg->complex_type(),
        shifter->value_type(),
        bundle->collector.type_registry,
        &bundle->collector.functions).has_value());
}

TEST_CASE("what leaves a requirement unmet", "[conformance]")
{
    SECTION("no member of that name") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f() : void; }\n"
            "struct S : I { int32 $x; }\n");
        REQUIRE(has_issue_containing(*bundle, "it declares no 'f'"));
    }

    SECTION("the return type differs") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f() : float64; }\n"
            "struct S : I { int32 $x; function f() : int32 { return 1; } }\n");
        REQUIRE(has_issue_containing(*bundle, "the closest it declares is"));
    }

    SECTION("a parameter type differs") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f(int32 $n) : void; }\n"
            "struct S : I { int32 $x; function f(float64 $n) : void { echo 1; } }\n");
        REQUIRE(has_issue_containing(*bundle, "the closest it declares is"));
    }

    SECTION("the arity differs") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface I { function f(int32 $n) : void; }\n"
            "struct S : I { int32 $x; function f() : void { echo 1; } }\n");
        REQUIRE(has_issue_containing(*bundle, "the closest it declares is"));
    }

    // the substituted form is what a diagnostic must render: the declared one still says `T`, a
    // parameter the author of the *implementor* never wrote and cannot act on
    SECTION("a generic requirement names the bound type, not its own parameter") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "interface Sized<T> { function first() : T; }\n"
            "struct Bad : Sized<int32> { int32 $x; function first() : float64 { return 1.0; } }\n");
        REQUIRE(has_issue_containing(*bundle, "first() : int32"));
    }
}

// **this is the payoff.** a constraint atom naming an interface is satisfied by conformance rather than
// by identity, which is the one thing a concrete-set constraint could never express. the rule has a
// single owner, so this is the only place the arm exists
TEST_CASE("an interface constraint admits every conforming type and no other", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Drawable { function draw() : void; }\n"
        "struct Square : Drawable { float64 $s; function draw() : void { echo 1; } }\n"
        "struct Rock { int32 $mass; }\n"
        "function render<T: Drawable>(T& $shape) : void { $shape->draw(); }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *drawable = type_named(m, "Drawable");
    auto *square = type_named(m, "Square");
    auto *rock = type_named(m, "Rock");
    REQUIRE(drawable != nullptr);

    auto decls = EchoTests::decls_named(m, "render");
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0]->type_parameters.size() == 1);

    const TypeParamDecl *param = decls[0]->type_parameters[0];
    REQUIRE(param->is_constrained());

    // the spelling is what the diagnostic renders, and it is the atom the user wrote
    REQUIRE(param->constraint_spelling == "Drawable");

    REQUIRE(param->allows(square->value_type()));
    REQUIRE_FALSE(param->allows(rock->value_type()));

    // a primitive answers no through the same arm rather than by a separate rule
    REQUIRE_FALSE(param->allows(prim(ValueTypePrimitive::t_int32)));

    // const is stripped before the comparison, exactly as it is for a concrete atom
    REQUIRE(param->allows(ValueType::make_const(square->value_type())));

    // ...and first_constraint_violation, the one predicate over `allows`, needed no arm of its own
    REQUIRE_FALSE(AST::first_constraint_violation(
        decls[0]->type_parameters, { square->value_type() }).has_value());
    REQUIRE(AST::first_constraint_violation(
        decls[0]->type_parameters, { rock->value_type() }) == 0u);
}

TEST_CASE("a bare generic interface is refused as a constraint atom", "[conformance]")
{
    // `Sized` alone resolves to the *template*, which is not a type any value has - so a constraint
    // naming one would reject every argument while looking perfectly correct
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Sized<T> { function first() : T; }\n"
        "function head<C: Sized>(C& $c) : int32 { return 1; }\n");

    REQUIRE(has_issue_containing(*bundle, "needs its type arguments in the constraint"));
}

TEST_CASE("a generic application is a legal constraint atom", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Sized<T> { function first() : T; }\n"
        "struct Bag<E> : Sized<E> { E $item; function first() : E { return $this->item; } }\n"
        "function head<C: Sized<int32>>(C& $c) : int32 { return $c->first(); }\n"
        "$b = Bag<int32>(7);\n"
        "echo head($b);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the call above instantiates `head`, so the arena holds the template *and* its instance. the
    // constraint lives on the template - an instance's arguments are already concrete
    const FunctionDeclNode *tmpl = nullptr;
    for (auto *decl : EchoTests::decls_named(m, "head")) {
        if (decl->is_generic()) {
            tmpl = decl;
        }
    }
    REQUIRE(tmpl != nullptr);

    const TypeParamDecl *param = tmpl->type_parameters[0];
    REQUIRE(param->is_constrained());
    REQUIRE(param->constraint.size() == 1);
    REQUIRE(param->constraint[0].is_interface());

    // the spelling is the rendered type, since an applied atom has no single token to quote
    REQUIRE(param->constraint_spelling == "Sized<int32>");
}
