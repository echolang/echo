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
    REQUIRE(fp == 0x285d02ec8e896b78ull);

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
    REQUIRE(AST::type_exports_abi_table(*english));
    REQUIRE_FALSE(AST::type_exports_abi_table(*plain));
    REQUIRE_FALSE(AST::type_exports_abi_table(*orphan));
    REQUIRE(AST::abi_table_export_refusal(*orphan, false).has_value());
    REQUIRE_FALSE(AST::abi_table_export_refusal(*english, false).has_value());
    REQUIRE(AST::abi_table_export_refusal(*english, true).has_value());
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
