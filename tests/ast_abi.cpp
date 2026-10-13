#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAbi.h>
#include <AST/ASTExternInterface.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using EchoTests::type_named;

TEST_CASE("abi fingerprint is stable for a fixture interface", "[extern_interface][abi]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "    function answer() : int32;\n"
        "}\n");

    auto &m = bundle->modules.find_module("test");
    auto *greeter = type_named(m, "greeter");
    REQUIRE(greeter != nullptr);

    const uint64_t fp = AST::abi_fingerprint(greeter->complex_type());
    REQUIRE(fp == 0x57943577daa1f67ull);

    REQUIRE(AST::abi_table_symbol(greeter->complex_type()) == "__eco_abi_7greeter");
    REQUIRE(AST::k_abi_version == 1);
}

TEST_CASE("abi fingerprint moves when a C alias changes", "[extern_interface][abi]")
{
    auto a = EchoTests::tests_make_parsed_bundle(
        "public extern interface sqlite {\n"
        "    function sqlite3_open as open(ptr<uint8> $path) : int32;\n"
        "}\n");
    auto b = EchoTests::tests_make_parsed_bundle(
        "public extern interface sqlite {\n"
        "    function sqlite3_open_v2 as open(ptr<uint8> $path) : int32;\n"
        "}\n");

    auto *ga = type_named(a->modules.find_module("test"), "sqlite");
    auto *gb = type_named(b->modules.find_module("test"), "sqlite");
    REQUIRE(ga != nullptr);
    REQUIRE(gb != nullptr);
    REQUIRE(AST::abi_fingerprint(ga->complex_type()) != AST::abi_fingerprint(gb->complex_type()));
}

TEST_CASE("type_exports_abi_table is export plus an extern conformance", "[extern_interface][abi]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "}\n"
        "#[export]\n"
        "public struct English : greeter {\n"
        "    static function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n"
        "}\n"
        "public struct Plain : greeter {\n"
        "    static function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n"
        "}\n"
        "#[export]\n"
        "public struct Orphan {\n"
        "    int32 $n;\n"
        "}\n");

    auto &m = bundle->modules.find_module("test");
    auto *english = type_named(m, "English");
    auto *plain = type_named(m, "Plain");
    auto *orphan = type_named(m, "Orphan");
    REQUIRE(english != nullptr);
    REQUIRE(plain != nullptr);
    REQUIRE(orphan != nullptr);
    REQUIRE(AST::type_exports_abi_table(*english, true));
    REQUIRE_FALSE(AST::type_exports_abi_table(*english, false));
    REQUIRE_FALSE(AST::type_exports_abi_table(*plain, true));
    REQUIRE_FALSE(AST::type_exports_abi_table(*orphan, true));
    REQUIRE(AST::abi_table_export_refusal(*orphan, true).has_value());
    REQUIRE_FALSE(AST::abi_table_export_refusal(*english, true).has_value());
    REQUIRE(AST::abi_table_export_refusal(*english, false).has_value());
}

TEST_CASE("abi fingerprint moves when a requirement name changes", "[extern_interface][abi]")
{
    auto a = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "}\n");
    auto b = EchoTests::tests_make_parsed_bundle(
        "public extern interface greeter {\n"
        "    function sum(int32 $a, int32 $b) : int32;\n"
        "}\n");

    auto *ga = type_named(a->modules.find_module("test"), "greeter");
    auto *gb = type_named(b->modules.find_module("test"), "greeter");
    REQUIRE(ga != nullptr);
    REQUIRE(gb != nullptr);
    REQUIRE(AST::abi_fingerprint(ga->complex_type()) != AST::abi_fingerprint(gb->complex_type()));
}

namespace
{
    uint64_t fingerprint_of(const std::string &source, const std::string &name)
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(source);
        auto *decl = type_named(bundle->modules.find_module("test"), name);
        REQUIRE(decl != nullptr);
        return AST::abi_fingerprint(decl->complex_type());
    }
}

// fingerprint and signatures_match answer the same question
TEST_CASE("abi fingerprint moves when a pointee gains const", "[extern_interface][abi]")
{
    const uint64_t mutable_buffer = fingerprint_of(
        "public extern interface reader {\n"
        "    function fill(ptr<uint8> $buffer) : int32;\n"
        "}\n", "reader");
    const uint64_t const_buffer = fingerprint_of(
        "public extern interface reader {\n"
        "    function fill(ptr<const uint8> $buffer) : int32;\n"
        "}\n", "reader");

    REQUIRE(mutable_buffer != const_buffer);
}

TEST_CASE("abi fingerprint ignores const on a by-value parameter", "[extern_interface][abi]")
{
    const uint64_t plain = fingerprint_of(
        "public extern interface greeter {\n"
        "    function add(int32 $a, int32 $b) : int32;\n"
        "}\n", "greeter");
    const uint64_t constant = fingerprint_of(
        "public extern interface greeter {\n"
        "    function add(const int32 $a, int32 $b) : int32;\n"
        "}\n", "greeter");

    REQUIRE(plain == constant);
}
