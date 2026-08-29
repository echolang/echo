#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConstness.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/TypeNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

TEST_CASE("a method is registered on its type, not in the enclosing namespace", "[methods]")
{
    // the whole difference between register_member_function and register_function. a method is
    // reached through a receiver, so a bare `sum(...)` must not find it - otherwise every struct in
    // a program would contribute its member names to the file's overload sets
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function sum() : int32 { return $this->x; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    // on the type
    auto members = find_member_functions(&point->complex_type(), "sum");
    REQUIRE(members.size() == 1);
    REQUIRE(members[0]->is_member());
    REQUIRE(members[0]->owner_type == &point->complex_type());

    // and nowhere in the namespace
    REQUIRE(bundle->collector.functions.overloads("sum", *point->ast_namespace).empty());
}

TEST_CASE("a method's receiver is its first parameter, as a borrow of the struct", "[methods]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function shift(int32 $by) : void { $this->x = $this->x + $by; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "shift");
    REQUIRE(decls.size() == 1);

    auto *method = decls[0];

    // the receiver is a real parameter rather than a body-local the way a constructor's $this is,
    // which is what makes a method an ordinary function everywhere downstream
    REQUIRE(method->args.size() == 2);
    REQUIRE(method->implicit_arg_count() == 1);
    REQUIRE(method->args[0]->name() == "this");

    // a *borrow*, so the method writes through to the caller's storage. `Point&`, not `ptr<Point>`:
    // a receiver is never null, and a non-nullable parameter is what auto-borrows an argument
    const auto self_type = method->args[0]->type();
    REQUIRE(self_type.is_pointer());
    REQUIRE_FALSE(self_type.is_nullable());
    REQUIRE(self_type.pointee() == type_named(m, "Point")->value_type());
}

TEST_CASE("a method call passes the receiver's address as its first argument", "[methods]")
{
    // taken in the parser rather than left to the borrow coercion, because that rule reads a value
    // against a borrow parameter - a pointer receiver would rank as no fit at all and `$p->m()`
    // would fail where `$p->x` works
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function sum() : int32 { return $this->x; }\n"
        "}\n"
        "$p = Point(1);\n"
        "echo $p->sum();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "sum");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->arguments.size() == 1);
    REQUIRE(calls[0]->arguments[0]->get_node_type() == NodeType::n_expr_addrof);
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE(calls[0]->decl->is_member());
}

TEST_CASE("a pointer receiver is dereferenced to the struct before its address is taken", "[methods]")
{
    // `->` reaches through every pointer level. a `ptr<Point>` receiver has to become `Point&`, not
    // `ptr<ptr<Point>>`, so the parser spells out one deref per level under the address-of
    //
    // and that address-of is the promotion: borrowing a raw pointee is the step `unsafe` marks, which
    // is why the call is written inside a block. the shape it takes here is what the borrow is *made
    // of*, and the word changes none of it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function sum() : int32 { return $this->x; }\n"
        "}\n"
        "$p = Point(1);\n"
        "ptr<Point> $q = &$p;\n"
        "unsafe { echo $q->sum(); }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "sum");
    REQUIRE(calls.size() == 1);

    auto *receiver = calls[0]->arguments[0];
    REQUIRE(receiver->get_node_type() == NodeType::n_expr_addrof);

    // one deref for the one pointer level, so what reaches the parameter is a Point& either way
    auto *addr = static_cast<AddrOfExprNode *>(receiver);
    REQUIRE(addr->operand->get_node_type() == NodeType::n_expr_deref);
    REQUIRE(receiver->result_type() == calls[0]->decl->args[0]->type());
}

TEST_CASE("a member followed by '<' is still a comparison", "[methods]")
{
    // the one place a member call cannot copy the free-call rule. `foo <` is unambiguously a type
    // argument list because a bare identifier is never a comparison operand - values carry a `$` -
    // but a *member* is a perfectly good operand, so the list is parsed speculatively and rolled
    // back unless a `(` follows it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function sum() : int32 { return $this->x; }\n"
        "}\n"
        "$p = Point(1);\n"
        // the right side is a *variable* rather than the literal this case used to write: a `bool`
        // destination retypes a literal operand, so `bool $b = $p->x < 3;` compares an int32 against a
        // bool - which has no lowering and is now reported as one. that is a bug about literal typing
        // and not about the `<` this case is here for, so it is kept out of the way
        "int32 $limit = 3;\n"
        "bool $b = $p->x < $limit;\n");

    for (const auto &issue : bundle->collector.issues) {
        INFO("unexpected issue: " << issue->message());
    }
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // nothing was called, and the comparison survived
    REQUIRE(calls_to(m, "x").empty());
    REQUIRE(m.nodes.of_type<BinaryExprNode>().size() == 1);
}

TEST_CASE("a method of a generic struct carries its owner's type parameters ahead of its own", "[methods][generics]")
{
    // one substitution has to bind both, so they live in one list - the owner's first, because
    // TypeSubstitution::positional is positional over the whole thing. the declarations are shared
    // with the struct rather than re-declared: a TypeParamDecl has exactly one owner
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    function widen<U>(U $other) : U { return $other; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *box = type_named(m, "Box");
    REQUIRE(box != nullptr);

    auto decls = decls_named(m, "widen");
    REQUIRE(decls.size() == 1);
    auto *method = decls[0];

    REQUIRE(method->type_parameters.size() == 2);
    REQUIRE(method->inherited_type_param_count == 1);
    REQUIRE(method->own_type_param_count() == 1);

    // the *same* declaration the struct holds, not a copy. a copy would make the struct's `Box<T>`
    // and the method's receiver type two unequal applications
    REQUIRE(method->type_parameters[0] == box->type_parameters()[0]);
    REQUIRE(method->type_parameters[1]->name == "U");

    // and the two passes agree on it: re-minting U on the second pass is exactly what the
    // strip-then-prefix in parse_funcdecl exists to prevent, and it would show up here as a
    // parameter whose owner is not this declaration
    REQUIRE(method->type_parameters[1]->owner_func() == method);
}

TEST_CASE("a generic method resolves the owner's and its own parameter in one call", "[methods][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    function widen<U>(U $other) : U { return $other; }\n"
        "}\n"
        "$b = Box(7);\n"
        "echo $b->widen(1.5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "widen");
    REQUIRE(calls.size() == 1);

    auto *resolved = calls[0]->decl;
    REQUIRE(resolved != nullptr);

    // the call was rewired to a concrete instance carrying both type arguments: T from the receiver,
    // U from the argument. that is A8's "composition of two substitutions", and it needs no
    // composition at all once the receiver is a real parameter
    REQUIRE_FALSE(resolved->is_generic());
    REQUIRE(resolved->is_instantiated());
    REQUIRE(resolved->instantiation_args.size() == 2);
    REQUIRE(resolved->instantiation_args[0] == EchoTests::prim(ValueTypePrimitive::t_int32));
    REQUIRE(resolved->instantiation_args[1] == EchoTests::prim(ValueTypePrimitive::t_float64));

    // an instance is concrete, so it claims no inherited parameters of its own
    REQUIRE(resolved->inherited_type_param_count == 0);

    // but it is still a member, which is what keeps the owner segment in its mangled name
    REQUIRE(resolved->is_member());
}

TEST_CASE("explicit type arguments on a member call bind only the method's own parameters", "[methods][generics]")
{
    // `$b->widen<int32>(9)` says nothing about Box's T, which the receiver already fixes. so the
    // list is arity-checked against the *own* count - against the whole list it would demand two
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    function widen<U>(U $other) : U { return $other; }\n"
        "}\n"
        "$b = Box(2.5);\n"
        "echo $b->widen<int32>(9);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "widen");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->explicit_type_args.size() == 1);

    auto *resolved = calls[0]->decl;
    REQUIRE(resolved != nullptr);
    REQUIRE(resolved->instantiation_args.size() == 2);

    // T inferred from the receiver, U taken from the list
    REQUIRE(resolved->instantiation_args[0] == EchoTests::prim(ValueTypePrimitive::t_float64));
    REQUIRE(resolved->instantiation_args[1] == EchoTests::prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("a method's mangled name is qualified by its owner", "[methods]")
{
    // without the owner segment a method `Foo::get()` and a free `get(Foo& $f)` mangle identically.
    // a method is deliberately absent from the (namespace, name) overload sets, so
    // DuplicateFunctionSignature cannot catch that - it would surface as TypeLowering's
    // "this is a name mangling defect, not a source error" throw
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo {\n"
        "    int32 $v;\n"
        "    function get() : int32 { return $this->v; }\n"
        "}\n"
        "function get(Foo& $f) : int32 { return $f->v; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "get");
    REQUIRE(decls.size() == 2);

    REQUIRE(decls[0]->decorated_func_name() != decls[1]->decorated_func_name());
}

TEST_CASE("a method's rendered signature shows neither the receiver nor the owner's parameters", "[methods]")
{
    // this string reaches NoMatchingOverload, AmbiguousCall, DuplicateFunctionSignature and the
    // debug dumps, so a leaked `$this` would be visible in every diagnostic a user ever sees
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    T $value;\n"
        "    function widen<U>(U $other) : U { return $other; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "widen");
    REQUIRE(decls.size() == 1);

    REQUIRE(decls[0]->signature_description() == "Box::widen<U>(U)");
}

TEST_CASE("a method can be called before it is declared", "[methods]")
{
    // the declaration pass takes method signatures out of a struct body, so that `$this->log()`
    // resolves when `log` is written below its caller - the forward reference free functions and
    // struct types already get
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Counter {\n"
        "    int32 $count;\n"
        "    function bump() : int32 { return $this->log(); }\n"
        "    function log() : int32 { return $this->count; }\n"
        "}\n"
        "$c = Counter(1);\n"
        "echo $c->bump();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "log");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
}

TEST_CASE("reaching a struct body in both parse passes does not duplicate constructors", "[methods]")
{
    // the reason the declaration pass used to take *only* method signatures from a struct body:
    // a constructor's declaration site was a name token minted where
    // the struct's name is written, so a second pass minted a second one and registered the same
    // constructor again as a duplicate signature. it is now the `constructor` keyword - a real token
    // at a fixed index - and both passes walk the whole body. a written constructor also deletes
    // memberwise, so the user's is the only one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $v) { $this->x = $v; $this->y = $v; }\n"
        "    function sum() : int32 { return $this->x + $this->y; }\n"
        "}\n"
        "$p = Point(3);\n"
        "echo $p->sum();\n");

    REQUIRE_FALSE(has_issue_containing(*bundle, "already declared"));
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    // the user's, exactly once - not one node per pass, and not a surviving memberwise beside it
    REQUIRE(point->constructors().size() == 1);
    REQUIRE(point->constructors()[0]->args.size() == 1);
    REQUIRE(point->constructors()[0]->body != nullptr);
    REQUIRE(point->synthesized_constructor() == nullptr);
}

TEST_CASE("two methods with the same parameter types are rejected", "[methods]")
{
    // the member counterpart of the duplicate-signature check. it has to key on the owner rather
    // than a namespace, because that is where a method is reachable from
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function add(int32 $a) : int32 { return $a; }\n"
        "    function add(int32 $b) : int32 { return $b; }\n"
        "}\n");

    // rendered from the owner, and without the receiver the caller never wrote
    REQUIRE(has_issue_containing(*bundle, "'Point::add(int32)' is already declared"));
}

TEST_CASE("calling a member function a type does not declare is reported", "[methods]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "}\n"
        "$p = Point(1);\n"
        "echo $p->nope();\n");

    REQUIRE(has_issue_containing(*bundle, "has no member named 'nope'"));
}

TEST_CASE("a method is not reachable as a free function", "[methods]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function sum() : int32 { return $this->x; }\n"
        "}\n"
        "$p = Point(1);\n"
        "echo sum($p);\n");

    // UnknownFunction, not "no overload accepts these arguments": the name is not declared in the
    // namespace at all, which is the whole point of keeping methods out of the overload sets
    REQUIRE(has_issue_containing(*bundle, "The function 'sum' could not be found"));
}

TEST_CASE("a member call diagnostic counts the arguments the caller wrote", "[methods]")
{
    // the receiver occupies parameter 0, so a raw index would report the user's only argument as
    // "argument 2" - a number they cannot find in their own source. the null-into-borrow guard is
    // the reachable member of that family: a plain type mismatch is wrapped in an implicit cast at
    // the call site and reported by visitTypeCast instead, which names no index at all
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    function set(int32& $r) : void { $r = $this->x; }\n"
        "}\n"
        "$p = Point(1);\n"
        "$p->set(null);\n");

    REQUIRE(has_issue_containing(*bundle, "argument 1 of 'set'"));
}

TEST_CASE("a const method's receiver is a const borrow of the struct", "[methods][const]")
{
    // a method's const-ness *is* args[0]'s type - there is no
    // flag on the declaration - so this assertion is the feature, and everything downstream (the
    // mangler, argument_fit, member access) reads it off here
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    const function get() : int32 { return $this->x; }\n"
        "    function shift(int32 $by) : void { $this->x = $this->x + $by; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    auto readers = decls_named(m, "get");
    REQUIRE(readers.size() == 1);

    const auto self_type = readers[0]->args[0]->type();
    REQUIRE(self_type.is_pointer());
    REQUIRE_FALSE(self_type.is_nullable());
    REQUIRE(self_type.pointee().is_const());
    REQUIRE(AST::receiver_is_const(*readers[0]));

    // the const is on the *pointee*, so `const Foo&` and not `const (Foo&)` - the shape a written
    // `const Point&` parameter produces, which is what makes the two one type
    REQUIRE_FALSE(self_type.is_const());
    REQUIRE(self_type.pointee() == ValueType::make_const(type_named(m, "Point")->value_type()));

    // and the mutable receiver is untouched, so the two share nothing but the struct
    auto writers = decls_named(m, "shift");
    REQUIRE(writers.size() == 1);
    REQUIRE_FALSE(AST::receiver_is_const(*writers[0]));
    REQUIRE_FALSE(writers[0]->args[0]->type().pointee().is_const());
}

TEST_CASE("a method may be overloaded on its receiver's const-ness", "[methods][const]")
{
    // two signatures, not a DuplicateFunctionSignature: `$this` is args[0], so the mangler already
    // tells them apart, and the matcher picks by the receiver - t_borrow for the one whose const-ness
    // matches, t_borrow_const for the one that adds it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag {\n"
        "    int32 $x;\n"
        "    function at() : int32& { return &$this->x; }\n"
        "    const function at() : const int32& { return &$this->x; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto members = find_member_functions(&type_named(m, "Bag")->complex_type(), "at");
    REQUIRE(members.size() == 2);

    REQUIRE(members[0]->decorated_func_name() != members[1]->decorated_func_name());

    // exactly one of them is the reader, whichever order the lookup hands them back in
    REQUIRE((AST::receiver_is_const(*members[0]) != AST::receiver_is_const(*members[1])));

    // and the signature a diagnostic renders tells them apart, which is what keeps a "no overload"
    // message from listing the same line twice
    REQUIRE(members[0]->signature_description() != members[1]->signature_description());
}

TEST_CASE("only a method may be declared const", "[methods][const]")
{
    // `const` qualifies a receiver, and a free function has none. reported and dropped rather than
    // refused, so the declaration still registers and the call below is not a second, unrelated
    // "unknown function"
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const function reads(int32 $n) : int32 { return $n; }\n"
        "echo reads(1);\n");

    REQUIRE(has_issue_containing(*bundle, "is not a method, so it cannot be declared const"));

    auto decls = decls_named(bundle->modules.find_module("test"), "reads");
    REQUIRE(decls.size() == 1);
    REQUIRE_FALSE(AST::receiver_is_const(*decls[0]));
}

TEST_CASE("a generic struct's const method keeps the qualifier through substitution", "[methods][const]")
{
    // const-ness is decided once, on the template's receiver, so two instances agree about which
    // methods only read - `find_member_functions`' template_ref redirect would stop meaning anything
    // if `Holder<int32>::peek` and `Holder<float64>::peek` had different signatures
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Holder<T> {\n"
        "    T $item;\n"
        "    constructor(T $v) { $this->item = $v; }\n"
        "    const function peek() : T { return $this->item; }\n"
        "}\n"
        "const $a = Holder<int32>(1);\n"
        "const $b = Holder<float64>(2.5);\n"
        "echo $a->peek();\n"
        "echo $b->peek();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "peek");
    REQUIRE(calls.size() == 2);

    for (auto *call : calls) {
        REQUIRE(call->decl != nullptr);

        // the instance, not the template - and still a reader
        REQUIRE_FALSE(call->decl->is_generic());
        REQUIRE(AST::receiver_is_const(*call->decl));
        REQUIRE(call->decl->args[0]->type().pointee().is_const());
    }

    // and the two instances really are distinct, so this says something about substitution rather
    // than about one declaration seen twice
    REQUIRE(calls[0]->decl != calls[1]->decl);
}
