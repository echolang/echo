#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTExternInterface.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

TEST_CASE("an extern interface is a declared type whose values are structs", "[extern_interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "    function answer() : int32;\n"
        "}\n");
    // Catch2 helpers parse without the stdlib, so `erased` is unbound and the
    // declaration is refused at typecheck. the kind and slots are still there
    REQUIRE(EchoTests::has_issue_containing(*bundle, "needs the standard library"));

    auto &m = bundle->modules.find_module("test");
    auto *greeter = type_named(m, "greeter");
    REQUIRE(greeter != nullptr);

    REQUIRE(greeter->kind() == ComplexTypeKind::t_extern_interface);
    REQUIRE(greeter->complex_type().is_extern_interface_kind());
    REQUIRE(std::string(AST::type_kind_keyword(greeter->kind())) == "extern interface");
    REQUIRE(greeter->value_type().is_struct());
    REQUIRE(greeter->value_type().has_property_layout());
    REQUIRE_FALSE(greeter->complex_type().has_memberwise_construction());

    const auto &methods = greeter->complex_type().methods();
    REQUIRE(methods.size() == 2);
    REQUIRE(methods[0]->func_name() == "add");
    REQUIRE(methods[1]->func_name() == "answer");
    REQUIRE(methods[0]->body != nullptr);
    REQUIRE(methods[0]->body_is_synthesized);
    REQUIRE_FALSE(methods[0]->is_implicitly_generated);

    REQUIRE(greeter->complex_type().property_count() == 3);
    REQUIRE(greeter->properties()[0]->name() == AST::extern_slot_property_name(0, "add"));
    REQUIRE(greeter->properties()[1]->name() == AST::extern_slot_property_name(1, "answer"));
    REQUIRE(greeter->properties()[2]->name() == AST::k_extern_image_property);
}

TEST_CASE("a requirement alias stores the C name on abi_link_name", "[extern_interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface sqlite {\n"
        "    function sqlite3_open as open(ptr<uint8> $path) : int32;\n"
        "}\n");

    auto *sqlite = type_named(bundle->modules.find_module("test"), "sqlite");
    REQUIRE(sqlite != nullptr);
    REQUIRE(sqlite->complex_type().methods().size() == 1);
    REQUIRE(sqlite->complex_type().methods()[0]->func_name() == "open");
    REQUIRE(sqlite->complex_type().methods()[0]->abi_link_name == "sqlite3_open");
}

TEST_CASE("a user property named like an extern slot is still typechecked", "[extern_interface][sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern struct Handle;\n"
        "public struct Box {\n"
        "    Handle $__state;\n"
        "}\n");
    REQUIRE(has_issue_containing(*bundle, "incomplete type"));
}

TEST_CASE("an exported conformer is refused on a wasm row", "[extern_interface][export]")
{
    Compiler::CompilerOptions options = EchoTests::tests_compiler_options();
    options.codegen = Compiler::CodegenTarget::wasi();
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "}\n"
        "#[export]\n"
        "public struct Host : greeter {\n"
        "    static function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n"
        "}\n",
        options);
    REQUIRE(has_issue_containing(*bundle, "cannot export an extern-interface table from a wasm module"));
}

TEST_CASE("an extern interface refuses a static requirement", "[extern_interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    static function add(int32 $a, int32 $b) : int32;\n"
        "}\n");
    REQUIRE(EchoTests::has_issue_containing(*bundle, "cannot be static"));
}

TEST_CASE("an operator inside an extern interface is refused as a slot, not as a struct member", "[extern_interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    operator (int32 $a) + (int32 $b) : int32;\n"
        "}\n");
    REQUIRE(has_issue_containing(*bundle, "extern interface cannot declare an operator"));
    REQUIRE_FALSE(has_issue_containing(*bundle, "inside a struct"));
}

TEST_CASE("an extern interface refuses an associated type", "[extern_interface]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    type Iter;\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "}\n");
    REQUIRE(EchoTests::has_issue_containing(*bundle, "associated type"));
}
