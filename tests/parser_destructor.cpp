#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTBundle.h>
#include <AST/ASTDestruction.h>
#include <AST/ASTMangler.h>
#include <AST/ASTMemberLookup.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/TypeNode.h>
#include <AST/VarDeclNode.h>

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

namespace
{
    // the smallest owning type: a raw pointer plus a destructor. the leaf case of every owning type
    // in the language, and the one the copy rule turns on.
    //
    // the destructor bodies here are deliberately free of library calls: the unit-test harness parses
    // a single file with no stdlib module, so a `mem::` call would be an unknown function and every
    // assertion below would be drowned in that diagnostic. what is under test is the declaration, and
    // the real release-the-buffer behaviour is covered end to end in tests_eco/ownership/
    const char *k_buffer =
        "struct Buffer {\n"
        "    ptr<uint8> $data;\n"
        "    destructor() { $this->data = null; }\n"
        "}\n";
}

TEST_CASE("a destructor is registered on its type, in neither lookup structure", "[destructor]")
{
    // it is not a name a call site can spell - `destructor` is a keyword - so it must not turn up as
    // an overload candidate either in the namespace or in the owner's method table. the drop sites
    // the ownership pass inserts reach it through find_destructor and nothing else
    auto bundle = EchoTests::tests_make_parsed_bundle(k_buffer);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *buffer = type_named(m, "Buffer");
    REQUIRE(buffer != nullptr);

    auto *dtor = find_destructor(&buffer->complex_type());
    REQUIRE(dtor != nullptr);
    REQUIRE(dtor->is_destructor());
    REQUIRE(dtor->owner_type == &buffer->complex_type());
    REQUIRE(dtor->body != nullptr);

    // not in the method table
    REQUIRE(buffer->complex_type().methods().empty());
    REQUIRE(find_member_functions(&buffer->complex_type(), "destructor").empty());

    // and not in the namespace's overload sets
    REQUIRE(bundle->collector.functions.overloads("destructor", *buffer->ast_namespace).empty());
}

TEST_CASE("a destructor's receiver is a borrow, and it is its only parameter", "[destructor]")
{
    // a borrow rather than a value, exactly as a method's is: a destructor over a *copy* would free
    // the copy's pointer and leave the caller's storage holding a dangling one
    auto bundle = EchoTests::tests_make_parsed_bundle(k_buffer);

    auto &m = bundle->modules.find_module("test");
    auto *dtor = find_destructor(&type_named(m, "Buffer")->complex_type());
    REQUIRE(dtor != nullptr);

    REQUIRE(dtor->args.size() == 1);
    REQUIRE(dtor->implicit_arg_count() == 1);
    REQUIRE(dtor->args[0]->name_full() == "$this");

    const ValueType receiver = dtor->args[0]->type();
    REQUIRE(receiver.is_pointer());
    REQUIRE_FALSE(receiver.is_nullable());
    REQUIRE(receiver.pointee().is_struct());

    REQUIRE(dtor->get_return_type().is_void());
}

TEST_CASE("a destructor's mangled name carries the owner segment", "[destructor]")
{
    // without it a destructor and a free function named `destructor` would mangle alike - and since a
    // destructor is deliberately absent from every overload set, DuplicateFunctionSignature cannot
    // see the clash, so it would surface as TypeLowering's mangling-defect throw
    auto bundle = EchoTests::tests_make_parsed_bundle(k_buffer);

    auto &m = bundle->modules.find_module("test");
    auto *dtor = find_destructor(&type_named(m, "Buffer")->complex_type());
    REQUIRE(dtor != nullptr);

    const std::string mangled = mangle_function_name(dtor);
    REQUIRE(mangled.find("M") != std::string::npos);
    REQUIRE(mangled.find("Buffer") != std::string::npos);
    REQUIRE(mangled.find("destructor") != std::string::npos);
}

TEST_CASE("a destructor renders as the user wrote it", "[destructor]")
{
    // this string reaches every diagnostic about the declaration, so the implicit receiver must not
    // appear in it
    auto bundle = EchoTests::tests_make_parsed_bundle(k_buffer);

    auto &m = bundle->modules.find_module("test");
    auto *dtor = find_destructor(&type_named(m, "Buffer")->complex_type());
    REQUIRE(dtor != nullptr);

    REQUIRE(dtor->signature_description() == "Buffer::destructor()");
}

TEST_CASE("a generic struct shares its type parameters with its destructor", "[destructor][generics]")
{
    // all of them inherited: a destructor has no parameters of its own to spell, and no call site to
    // spell them at. the receiver `Box<T>&` is what binds the owner's T from the local being dropped
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    ptr<T> $slot;\n"
        "    destructor() { $this->slot = null; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *box = type_named(m, "Box");
    REQUIRE(box != nullptr);

    auto *dtor = find_destructor(&box->complex_type());
    REQUIRE(dtor != nullptr);
    REQUIRE(dtor->is_generic());
    REQUIRE(dtor->type_parameters == box->type_parameters());
    REQUIRE(dtor->inherited_type_param_count == dtor->type_parameters.size());
    REQUIRE(dtor->own_type_param_count() == 0);
}

TEST_CASE("an instantiation finds its destructor through its template", "[destructor][generics]")
{
    // `Box<int32>` holds no destructor of its own - members belong to the template and are
    // instantiated per call site, so the lookup redirects exactly as find_member_functions does
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    ptr<T> $slot;\n"
        "    destructor() { $this->slot = null; }\n"
        "}\n"
        "$b = Box<int32>(null);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *box = type_named(m, "Box");
    REQUIRE(box != nullptr);

    // the interned application, reached from the registry rather than from a declaration
    const ComplexType *instance = nullptr;
    for (const ComplexType *ct : bundle->collector.type_registry.instantiations()) {
        if (ct->template_ref == &box->complex_type()) {
            instance = ct;
            break;
        }
    }

    REQUIRE(instance != nullptr);
    REQUIRE(instance->destructor() == nullptr);
    REQUIRE(find_destructor(instance) == find_destructor(&box->complex_type()));
}

TEST_CASE("needs_destruction is transitive through properties but stops at a pointer", "[destructor]")
{
    // "a struct that contains an owner is itself an owner, and nothing needs to be declared for
    // that." and the leaf: a raw pointer owns nothing as far as the type system can tell, which is
    // exactly why a type holding one has to say what to do in a destructor
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Buffer {\n"
        "    ptr<uint8> $data;\n"
        "    destructor() { $this->data = null; }\n"
        "}\n"
        "struct Wrap { Buffer $inner; usize $version; }\n"
        "struct Plain { usize $a; ptr<uint8> $b; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(needs_destruction(type_named(m, "Buffer")->value_type()));
    REQUIRE(needs_destruction(type_named(m, "Wrap")->value_type()));

    // no destructor anywhere in it, and its pointer field is not an owner
    REQUIRE_FALSE(needs_destruction(type_named(m, "Plain")->value_type()));

    // nor is a borrow of an owning type: a borrow does not keep anything alive, so it certainly does
    // not destroy anything
    REQUIRE_FALSE(needs_destruction(
        ValueType::make_pointer(type_named(m, "Buffer")->value_type(), false)));

    REQUIRE_FALSE(needs_destruction(EchoTests::prim(ValueTypePrimitive::t_int32)));
}

TEST_CASE("a destructor cannot take parameters, return a type, or be declared twice", "[destructor]")
{
    SECTION("parameters") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Buffer {\n"
            "    ptr<uint8> $data;\n"
            "    destructor(usize $extra) { }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "A destructor takes no parameters"));
    }

    SECTION("return type") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Buffer {\n"
            "    ptr<uint8> $data;\n"
            "    destructor() : void { }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "A destructor returns nothing"));
    }

    SECTION("declared twice") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Buffer {\n"
            "    ptr<uint8> $data;\n"
            "    destructor() { }\n"
            "    destructor() { }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "already has a destructor"));
    }
}

TEST_CASE("`destructor` is a whole-word keyword", "[destructor][lexer]")
{
    // the hazard ECHO_LEX_FNC_KEYWORD exists for: a plain string entry matches a *prefix*, which is
    // how `const` once ate the head of `constructor`. an identifier that merely starts with
    // `destructor` must still be an identifier
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function destructors_run() : int32 { return 1; }\n"
        "echo destructors_run();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}
