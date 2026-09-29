#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConformance.h>
#include <AST/ASTInstantiation.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ASTTypeParam.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include <vector>

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
    // Sized requires a method, so a primitive still answers false: structural conformance is
    // operator-only. the cases below pin that the total-false path is this refusal, not a crash
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

// a primitive has no declaration to opt in with, so an operator-only interface is answered by
// builtin meaning on the peeled operands. a declared type stays nominal: Gate below has `<` and
// still does not conform until it writes the clause
TEST_CASE("a type with no declaration conforms structurally to an operator-only interface", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Comparable<T> {\n"
        "    operator (const T& $a) < (const T& $b) : bool;\n"
        "}\n"
        "interface Addable<T> {\n"
        "    operator (T $a) + (T $b) : T;\n"
        "}\n"
        "interface Marker {}\n"
        "struct Gate {\n"
        "    int32 $n;\n"
        "}\n"
        "operator (const Gate& $a) < (const Gate& $b) : bool {\n"
        "    return $a->n < $b->n;\n"
        "}\n"
        "function lighter<T : Comparable<T>>(T $a, T $b) : bool {\n"
        "    return $a < $b;\n"
        "}\n"
        "function twice<T : Addable<T>>(T $x) : T {\n"
        "    return $x + $x;\n"
        "}\n"
        "echo lighter(1, 2);\n"
        "echo twice(3);\n"
        "echo lighter(1.5, 2.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *comparable = type_named(m, "Comparable");
    auto *addable = type_named(m, "Addable");
    auto *marker = type_named(m, "Marker");
    auto *gate = type_named(m, "Gate");
    REQUIRE(comparable != nullptr);
    REQUIRE(addable != nullptr);
    REQUIRE(marker != nullptr);
    REQUIRE(gate != nullptr);

    auto intern = [&](TypeDeclNode *iface, ValueType arg) {
        return ValueType::make_complex(
            bundle->collector.type_registry.get_or_create_instantiation(
                &iface->complex_type(), { arg }));
    };

    const ValueType int32 = prim(ValueTypePrimitive::t_int32);
    const ValueType float64 = prim(ValueTypePrimitive::t_float64);
    const ValueType boolean = prim(ValueTypePrimitive::t_bool);
    const ValueType ptr_int = ValueType::make_pointer(int32, false);

    REQUIRE(AST::conforms_to(int32, intern(comparable, int32)));
    REQUIRE(AST::conforms_to(float64, intern(comparable, float64)));
    REQUIRE_FALSE(AST::conforms_to(int32, intern(comparable, float64)));
    REQUIRE_FALSE(AST::conforms_to(boolean, intern(comparable, boolean)));

    REQUIRE(AST::conforms_to(int32, intern(addable, int32)));
    REQUIRE_FALSE(AST::conforms_to(boolean, intern(addable, boolean)));

    REQUIRE(AST::conforms_to(ptr_int, intern(comparable, ptr_int)));

    // an empty marker is something to opt into, so a primitive does not answer one
    REQUIRE_FALSE(AST::conforms_to(int32, marker->value_type()));

    // a struct stays nominal even when it already has `<`
    REQUIRE_FALSE(AST::conforms_to(gate->value_type(), intern(comparable, gate->value_type())));
}

// operators cannot live in a struct body and cannot inherit the implementor's type parameters, so
// `operator<E>` is the only spelling of `<` for a generic struct. the requirement has no own
// parameters (T belongs to the interface); the candidate has one. that used to drop the operator
// before types were compared
TEST_CASE("a generic struct opts into an operator interface with operator<E>", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Comparable<T> {\n"
        "    operator (const T& $a) < (const T& $b) : bool;\n"
        "}\n"
        "struct Box<E> : Comparable<Box<E>> {\n"
        "    E $item;\n"
        "}\n"
        "operator<E> (const Box<E>& $a) < (const Box<E>& $b) : bool {\n"
        "    return $a->item < $b->item;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *box = type_named(m, "Box");
    REQUIRE(box != nullptr);
    REQUIRE(box->complex_type().conformances().size() == 1);

    REQUIRE_FALSE(AST::first_unmet_requirement(
        &box->complex_type(),
        box->complex_type().conformances()[0],
        bundle->collector.type_registry,
        &bundle->collector.functions).has_value());
}

TEST_CASE("a generic struct is refused when its operator is over a different type", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Comparable<T> {\n"
        "    operator (const T& $a) < (const T& $b) : bool;\n"
        "}\n"
        "struct Box<E> : Comparable<Box<E>> {\n"
        "    E $item;\n"
        "}\n"
        "struct Gate {\n"
        "    int32 $n;\n"
        "}\n"
        "operator (const Gate& $a) < (const Gate& $b) : bool {\n"
        "    return $a->n < $b->n;\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "says it conforms to"));
}

TEST_CASE("a primitive is refused when an operator-only constraint does not hold", "[conformance]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Comparable<T> {\n"
        "    operator (const T& $a) < (const T& $b) : bool;\n"
        "}\n"
        "function lighter<T : Comparable<T>>(T $a, T $b) : bool {\n"
        "    return $a < $b;\n"
        "}\n"
        "echo lighter(true, false);\n");

    REQUIRE(has_issue_containing(*bundle, "constrained to 'Comparable<T>'"));
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
        decls[0]->type_parameters, { square->value_type() }, bundle->collector.type_registry)
        .violation.has_value());
    REQUIRE(AST::first_constraint_violation(
        decls[0]->type_parameters, { rock->value_type() }, bundle->collector.type_registry)
        .violation == 0u);
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

TEST_CASE("a requirement's vtable slot survives instantiation", "[conformance]")
{
    // **the slot lookup and the list it searches have to take the same redirect.**
    // AST::interface_requirements answers through `template_or_self()`, so its entries are always the
    // *template's* declarations - while a call site reaching a **generic** interface holds an instance of
    // the requirement, the monomorphizer making one per application. matching those by pointer identity
    // could never succeed, and the miss was not a wrong slot but an InternalCompilerException at the
    // dispatch site: `foreach` over an erased `contract::iterator<V>` took the compiler down, which is why
    // that arm had never actually run.
    //
    // a non-generic interface has no instantiation step, which is the whole of why erasing to a plain
    // `Drawable` always worked - so both shapes are asserted here, not only the one that was broken
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Plain\n"
        "{\n"
        "    function ping() : int32;\n"
        "    function pong() : int32;\n"
        "}\n"
        "interface Boxed<T>\n"
        "{\n"
        "    function first() : T;\n"
        "    function second() : T;\n"
        "}\n"
        "class Both : Plain, Boxed<int32>\n"
        "{\n"
        "    int32 $v;\n"
        "    public function ping() : int32 { return 1; }\n"
        "    public function pong() : int32 { return 2; }\n"
        "    public function first() : int32 { return 3; }\n"
        "    public function second() : int32 { return 4; }\n"
        "}\n"
        "function take_plain(Plain $p) : int32 { return $p->pong(); }\n"
        "function take_boxed(Boxed<int32> $b) : int32 { return $b->second(); }\n"
        "echo take_plain(Both(0));\n"
        "echo take_boxed(Both(0));\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");

    AST::TypeDeclNode *plain = EchoTests::type_named(module, "Plain");
    AST::TypeDeclNode *boxed = EchoTests::type_named(module, "Boxed");

    REQUIRE(plain != nullptr);
    REQUIRE(boxed != nullptr);

    // **every declaration a call could be carrying resolves to a slot**, including the instantiated
    // requirements the monomorphizer minted for `Boxed<int32>` - which are not the template's pointers
    size_t checked = 0;

    for (auto *decl : module.nodes.of_type<AST::FunctionDeclNode>()) {
        if (decl->owner_type == nullptr || !decl->owner_type->is_interface_kind()) {
            continue;
        }

        const std::optional<size_t> slot =
            AST::interface_method_slot(decl->owner_type, decl);

        REQUIRE(slot.has_value());
        REQUIRE(slot.value() < 2);
        checked++;
    }

    // the four requirements plus whatever instances were minted for the generic one; if this ever drops
    // to the two non-generic ones the case has stopped covering what it is named for
    REQUIRE(checked >= 4);

    // and the second requirement of each really is slot 1 - the order is declaration order, which is what
    // Compiler::LLVM::TypeLowering builds the table in
    const std::vector<AST::FunctionDeclNode *> &plain_reqs =
        AST::interface_requirements(&plain->complex_type());

    REQUIRE(plain_reqs.size() == 2);
    REQUIRE(AST::interface_method_slot(&plain->complex_type(), plain_reqs[1]).value() == 1);
}

TEST_CASE("a generic class instantiation can be stored as an interface", "[conformance]")
{
    // the vtable is built from the instance, not the template: a method of View<T> is instantiated
    // per call site, a vtable slot is not a call site, so the monomorphizer force-emits contains
    // when View<int32> is interned as a class that can be stored as Store
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Store { function contains(uint32 $e) : bool; }\n"
        "class View<T> : Store {\n"
        "    public uint32 $id;\n"
        "    public function contains(uint32 $e) : bool { return $e == $this->id; }\n"
        "}\n"
        "View<int32> $v = View<int32>(1);\n"
        "Store $s = $v;\n"
        "echo $s->contains(1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *view = type_named(m, "View");
    auto *store = type_named(m, "Store");
    REQUIRE(view != nullptr);
    REQUIRE(store != nullptr);

    auto *view_int = bundle->collector.type_registry.get_or_create_instantiation(
        &view->complex_type(), { prim(ValueTypePrimitive::t_int32) });

    const ValueType from = ValueType::make_class(view_int);
    const ValueType iface = store->value_type();

    REQUIRE(AST::interface_erasure_refusal(from, iface).empty());

    auto impls = AST::interface_implementations(view_int, iface, bundle->collector.type_registry);
    REQUIRE(impls.size() == 1);
    REQUIRE(impls[0] != nullptr);
    REQUIRE(impls[0]->is_instantiated());
    REQUIRE(impls[0]->instantiation_args == std::vector<ValueType>{ prim(ValueTypePrimitive::t_int32) });
}

TEST_CASE("force-emitting a vtable does not instantiate methods that are not requirements", "[conformance]")
{
    // the slot is contains. dump is an ordinary method of View<T>, and a vtable is not a reason
    // to emit it - only a call site is
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "interface Store { function contains(uint32 $e) : bool; }\n"
        "class View<T> : Store {\n"
        "    public uint32 $id;\n"
        "    public function contains(uint32 $e) : bool { return $e == $this->id; }\n"
        "    public function dump() : uint32 { return $this->id; }\n"
        "}\n"
        "View<int32> $v = View<int32>(1);\n"
        "Store $s = $v;\n"
        "echo $s->contains(1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *view = type_named(m, "View");
    REQUIRE(view != nullptr);

    const std::vector<ValueType> args { prim(ValueTypePrimitive::t_int32) };

    auto contains = AST::find_member_functions(&view->complex_type(), "contains");
    REQUIRE(contains.size() == 1);
    REQUIRE(contains[0]->instance_for(args) != nullptr);

    auto dumps = AST::find_member_functions(&view->complex_type(), "dump");
    REQUIRE(dumps.size() == 1);
    REQUIRE(dumps[0]->instance_for(args) == nullptr);
}
