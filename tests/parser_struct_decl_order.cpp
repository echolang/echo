#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

// a module is parsed in three passes - type names, then declarations, then bodies - and each one runs
// over every file before the next starts. what that buys is order independence: nothing a declaration
// names has to be written above it, or in a file listed before it. these are the cases that pin it

TEST_CASE("a property can be typed by a struct declared further down", "[structdecl]")
{
    // property types are read in the declaration pass, which is why the type *name* pass has to run
    // first. without it `Inner` is not a symbol yet and the property silently becomes `unknown` -
    // silently, because an unresolved unqualified type name is not a diagnostic
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Holder { Inner $inner; }\n"
        "struct Inner { int32 $v; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *holder = type_named(m, "Holder");
    REQUIRE(holder != nullptr);
    REQUIRE(holder->properties().size() == 1);

    REQUIRE(holder->properties()[0]->type().is_struct());
    REQUIRE(holder->properties()[0]->type() == type_named(m, "Inner")->value_type());
}

TEST_CASE("a property can be typed by a struct declared in a later file", "[structdecl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "struct Holder { Inner $inner; }\n",
        "struct Inner { int32 $v; }\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *holder = type_named(m, "Holder");
    REQUIRE(holder != nullptr);
    REQUIRE(holder->properties().size() == 1);
    REQUIRE(holder->properties()[0]->type() == type_named(m, "Inner")->value_type());
}

TEST_CASE("a property can be a generic application", "[structdecl]")
{
    // the composition case: a generic type that cannot be a property type is a generic type that
    // cannot be composed, so nothing built on `Array<T>` is reachable. assert the *layout*, not only
    // that it parsed - `L<int32>` has to intern a property of type `Q<int32>`, which is
    // substitute_type recursing into a generic application's arguments and re-interning
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Q<T> { T $x; }\n"
        "struct H { Q<int32> $i; }\n"
        "struct L<T> { Q<T> $i; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *q = type_named(m, "Q");
    auto *h = type_named(m, "H");
    auto *l = type_named(m, "L");
    REQUIRE(q != nullptr);
    REQUIRE(h != nullptr);
    REQUIRE(l != nullptr);

    const auto int32 = EchoTests::prim(AST::ValueTypePrimitive::t_int32);
    auto *q_of_int32 = bundle->collector.type_registry.get_or_create_instantiation(
        &q->complex_type(), { int32 });

    // the concrete application in a non-generic struct
    REQUIRE(h->properties().size() == 1);
    REQUIRE(h->properties()[0]->type() == AST::ValueType::make_complex(q_of_int32));

    // and the one that mentions the enclosing template's parameter. the *template's* property is
    // `Q<T>`, and instantiating L for int32 has to carry that through to `Q<int32>`
    REQUIRE(l->properties().size() == 1);
    REQUIRE(l->properties()[0]->type() != AST::ValueType::make_complex(q_of_int32));

    auto *l_of_int32 = bundle->collector.type_registry.get_or_create_instantiation(
        &l->complex_type(), { int32 });

    REQUIRE(l_of_int32->property_count() == 1);
    REQUIRE(l_of_int32->get_property(0).type == AST::ValueType::make_complex(q_of_int32));
}

TEST_CASE("a generic property type can be declared further down", "[structdecl]")
{
    // the same order independence the cases above pin for a plain property, for a generic one. it
    // needs one thing more: the type-name pass has to collect a generic type's *arity*, not only its
    // name, because parse_generic_application checks the application against the template's
    // parameter count. with only the name, `Box<int32>` read before `struct Box<T>` was reached
    // reported "wrong number of type arguments" against a template whose list nobody had read
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Holder { Box<int32> $b; }\n"
        "struct Box<T> { T $v; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *box = type_named(m, "Box");
    auto *holder = type_named(m, "Holder");
    REQUIRE(box != nullptr);
    REQUIRE(holder != nullptr);

    // the arity came from the type-name pass, before Holder's property was read
    REQUIRE(box->type_parameters().size() == 1);

    auto *box_of_int32 = bundle->collector.type_registry.get_or_create_instantiation(
        &box->complex_type(), { EchoTests::prim(AST::ValueTypePrimitive::t_int32) });

    REQUIRE(holder->properties().size() == 1);
    REQUIRE(holder->properties()[0]->type() == AST::ValueType::make_complex(box_of_int32));

    // and the layout behind it is complete, not the empty one the property's own intern saw
    REQUIRE(box_of_int32->property_count() == 1);
}

TEST_CASE("a struct's properties are collected by exactly one pass", "[structdecl]")
{
    // the body is walked in both the declaration and the body pass, by the same code so the two
    // cannot disagree about where a property ends - but only the first walk may keep what it parsed,
    // or the layout is doubled and every member after the first is at the wrong offset
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { int32 $x; int32 $y; }\n"
        "$p = Point(1, 2);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);
    REQUIRE(point->properties().size() == 2);
    REQUIRE(point->complex_type().property_count() == 2);
}

TEST_CASE("a constructor is one declaration across both parse passes", "[structdecl]")
{
    // the passes reconcile on the declaration site, which for a constructor is its own `constructor`
    // keyword - a real token at a fixed index. it used to be a token minted on the spot, meaning
    // "whichever index this pass happened to allocate", which only agreed as long as exactly one pass
    // ever reached a constructor. assert the reconciliation rather than that arrangement
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    constructor(int32 $v) { $this->x = $v; }\n"
        "    constructor(int32 $a, int32 $b) { $this->x = $a + $b; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    // two constructors, each one node - not one per pass, and not collapsed into one
    REQUIRE(point->constructors().size() == 2);
    REQUIRE(point->constructors()[0] != point->constructors()[1]);
    REQUIRE(point->constructors()[0]->args.size() == 1);
    REQUIRE(point->constructors()[1]->args.size() == 2);

    // and both carry the body the *body* pass gave them
    REQUIRE(point->constructors()[0]->body != nullptr);
    REQUIRE(point->constructors()[1]->body != nullptr);
    REQUIRE(point->constructors()[0]->body != point->constructors()[1]->body);

    // the field-wise one is a third declaration of the same name, suppressed here because
    // `constructor(int32)` already occupies the one-int32 signature this struct's single property
    // would produce
    REQUIRE(point->field_wise_constructor() == nullptr);
    REQUIRE(decls_named(m, "Point").size() == 2);
}

TEST_CASE("a constructor that never receives a body is reported", "[structdecl]")
{
    // the declaration pass skips a member body whole while the body pass parses it, so a malformed
    // method can send the two to different places and leave a registered constructor without a body
    // that has to be a located error: codegen *declares* every function in the module but emits a
    // body only for the ones in the file root, so it would otherwise link against nothing
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P {\n"
        "    int32 $x;\n"
        "    function m() : void { ) }\n"
        "    constructor(int32 $v) { $this->x = $v; }\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "was declared but never given a body"));
}
// two declarations of one type name. the parse passes reconcile on the declaration site, so a
// same-named struct declared somewhere *else* is not this pass revisiting the same struct - it is a
// redeclaration, and it has to be reported rather than folded into the first one, which is what
// members_collected() did on its own (silently keeping the first layout while the duplicate's methods
// and its second field-wise constructor still landed on the first struct)

namespace
{
    // how many diagnostics mention a redeclaration. a count, not a predicate, because the point of
    // most of these cases is that three passes over the same tokens yield exactly one
    size_t redeclaration_issue_count(const AST::Bundle &bundle)
    {
        size_t count = 0;
        for (const auto &issue : bundle.collector.issues) {
            if (issue->message().find("is already declared") != std::string::npos) {
                count++;
            }
        }
        return count;
    }

    size_t structs_named(AST::Module &m, const std::string &name)
    {
        size_t count = 0;
        for (auto *strct : m.nodes.of_type<AST::TypeDeclNode>()) {
            if (strct->type_name() == name) {
                count++;
            }
        }
        return count;
    }
}

TEST_CASE("a second struct of the same name is reported", "[structdecl]")
{
    EchoTests::assert_code_emits_issue(
        "struct Foo { int32 $x; }\n"
        "struct Foo { int32 $y; }\n",
        "The type 'Foo' is already declared on line 1 column 8. The first declaration is the one that is used");
}

TEST_CASE("the first declaration of a duplicated struct wins", "[structdecl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo { int32 $x; }\n"
        "struct Foo { int32 $y; }\n");

    REQUIRE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(structs_named(m, "Foo") == 1);

    auto *foo = type_named(m, "Foo");
    REQUIRE(foo != nullptr);
    REQUIRE(foo->properties().size() == 1);
    REQUIRE(foo->complex_type().property_count() == 1);
    REQUIRE(foo->properties()[0]->name() == "x");

    // one field-wise constructor, not two. the duplicate used to reach the tail of parse_typedecl and
    // push the same synthesized node into the file root a second time, which codegen then emitted a
    // second body for onto one llvm::Function
    REQUIRE(decls_named(m, "Foo").size() == 1);
}

TEST_CASE("a duplicated struct is reported exactly once", "[structdecl]")
{
    // both the declaration and the body pass reach the duplicate and both report at its own name
    // token, which is what Collector::collect_issue de-duplicates on
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo { int32 $x; }\n"
        "struct Foo { int32 $y; }\n");

    REQUIRE(redeclaration_issue_count(*bundle) == 1);
}

TEST_CASE("a struct duplicated in another file is reported", "[structdecl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "struct Foo { int32 $x; }\n",
        "struct Foo { int32 $y; }\n",
    });

    REQUIRE(redeclaration_issue_count(*bundle) == 1);

    auto &m = bundle->modules.find_module("test");
    REQUIRE(type_named(m, "Foo")->properties().size() == 1);
    REQUIRE(type_named(m, "Foo")->properties()[0]->name() == "x");
}

TEST_CASE("a duplicated struct's members do not leak into the first", "[structdecl]")
{
    // the duplicate's body is skipped whole rather than parsed and discarded: a method or a
    // constructor inside it reconciles on its *own* declaration site, so parsing it would register it
    // on the first struct. the skip is brace-depth aware, which the trailing struct pins
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo { int32 $x; }\n"
        "struct Foo {\n"
        "    int32 $y;\n"
        "    function m() : void {}\n"
        "    constructor(int32 $a) { $this->y = $a; }\n"
        "}\n"
        "struct Bar { int32 $z; }\n");

    REQUIRE(redeclaration_issue_count(*bundle) == 1);

    auto &m = bundle->modules.find_module("test");
    auto *foo = type_named(m, "Foo");
    REQUIRE(foo != nullptr);
    REQUIRE(foo->properties().size() == 1);
    REQUIRE(foo->methods().empty());
    REQUIRE(foo->constructors().empty());
    REQUIRE(foo->field_wise_constructor() != nullptr);

    // parsing resumed on the token after the duplicate's closing brace
    auto *bar = type_named(m, "Bar");
    REQUIRE(bar != nullptr);
    REQUIRE(bar->properties().size() == 1);
}

TEST_CASE("the same struct name in two namespaces is not a duplicate", "[structdecl]")
{
    // find_symbol looks only at the given namespace's own symbols, so this must stay legal
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "namespace a;\n"
        "struct Foo { int32 $x; }\n"
        "namespace b;\n"
        "struct Foo { int32 $y; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(structs_named(m, "Foo") == 2);
}

TEST_CASE("a struct declared inside a function body cannot redeclare a type", "[structdecl]")
{
    // passes 1 and 2 walk token by token, which is how a struct written inside a body is reached at
    // all - so this is the case where the most passes see the duplicate
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo { int32 $x; }\n"
        "function f() : void { struct Foo { int32 $y; } }\n");

    REQUIRE(redeclaration_issue_count(*bundle) == 1);

    auto &m = bundle->modules.find_module("test");
    REQUIRE(type_named(m, "Foo")->properties().size() == 1);
    REQUIRE(type_named(m, "Foo")->properties()[0]->name() == "x");
}

TEST_CASE("a duplicated generic struct does not gain the duplicate's type parameters", "[structdecl]")
{
    // the report has to come before declare_type_parameters, or the duplicate's <U, V> lands on the
    // first struct's ComplexType and every application of it is interned at the wrong arity
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo<T> { T $v; }\n"
        "struct Foo<U, V> { U $a; V $b; }\n");

    REQUIRE(redeclaration_issue_count(*bundle) == 1);

    auto &m = bundle->modules.find_module("test");
    auto *foo = type_named(m, "Foo");
    REQUIRE(foo != nullptr);
    REQUIRE(foo->type_parameters().size() == 1);
    REQUIRE(foo->properties().size() == 1);
}

// the forward reference a method already got is covered, more strictly, by "a method can be called
// before it is declared" in parser_methods.cpp - it asserts the call *resolved*, not just that both
// methods are on the type
