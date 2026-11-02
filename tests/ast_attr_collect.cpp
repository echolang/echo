#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAttrCollect.h>
#include <AST/ASTAttributeValue.h>
#include <AST/ASTCoreTypes.h>
#include <AST/ASTValueType.h>
#include <AST/AttributeNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

TEST_CASE("declarations_with_attribute finds a tagged function", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::get: \"/users\"]\n"
        "public function listUsers() : void {}\n"
        "public function ignored() : void {}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].kind == AST::AttrSubjectKind::t_function);
    REQUIRE(hits[0].function != nullptr);
    REQUIRE(hits[0].function->func_name() == "listUsers");
    REQUIRE(hits[0].attribute != nullptr);
    REQUIRE(hits[0].attribute->value.has_value());
    REQUIRE(hits[0].attribute->value->kind == AST::AttributeValueKind::t_string);
    REQUIRE(hits[0].attribute->value->text == "/users");
}

TEST_CASE("declarations_with_attribute finds a tagged type", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::middleware: \"auth\"]\n"
        "public class Auth {\n"
        "    int32 $n;\n"
        "}\n"
        "public struct Plain {\n"
        "    int32 $n;\n"
        "}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::middleware");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].kind == AST::AttrSubjectKind::t_type);
    REQUIRE(hits[0].type != nullptr);
    REQUIRE(hits[0].type->type_name() == "Auth");
}

TEST_CASE("declarations_with_attribute is source order across kinds", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::get: \"/a\"]\n"
        "public function first() : void {}\n"
        "#[http::get: \"/b\"]\n"
        "public struct Second {\n"
        "    int32 $n;\n"
        "}\n"
        "#[http::get: \"/c\"]\n"
        "public function third() : void {}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 3);
    REQUIRE(hits[0].kind == AST::AttrSubjectKind::t_function);
    REQUIRE(hits[0].function->func_name() == "first");
    REQUIRE(hits[1].kind == AST::AttrSubjectKind::t_type);
    REQUIRE(hits[1].type->type_name() == "Second");
    REQUIRE(hits[2].kind == AST::AttrSubjectKind::t_function);
    REQUIRE(hits[2].function->func_name() == "third");
}

TEST_CASE("declarations_with_attribute keeps file order", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string> {
        "#[http::get: \"/zero\"]\npublic function fromZero() : void {}\n",
        "#[http::get: \"/one\"]\npublic function fromOne() : void {}\n",
    });

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].function->func_name() == "fromZero");
    REQUIRE(hits[1].function->func_name() == "fromOne");
}

TEST_CASE("declarations_with_attribute ignores language attributes", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[inline]\n"
        "public function hot() : void {}\n"
        "#[http::get: \"/x\"]\n"
        "public function tagged() : void {}\n");

    REQUIRE(AST::declarations_with_attribute(*bundle, "inline").empty());
    REQUIRE(AST::declarations_with_attribute(*bundle, "http::get").size() == 1);
}

TEST_CASE("declarations_with_attribute skips tests", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle_with_tests(
        "#[http::get: \"/t\"]\n"
        "test aTaggedTest {\n"
        "}\n"
        "#[http::get: \"/f\"]\n"
        "public function kept() : void {}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].function->func_name() == "kept");
}

TEST_CASE("declarations_with_attribute finds a tagged method", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public struct Box {\n"
        "    #[http::get: \"/m\"]\n"
        "    public function hidden() : void {}\n"
        "}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].function->func_name() == "hidden");
    REQUIRE(hits[0].function->is_member());
}

TEST_CASE("an unknown collect name is empty, not every tag", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::get: \"/x\"]\n"
        "public function tagged() : void {}\n");

    REQUIRE(AST::declarations_with_attribute(*bundle, "http::post").empty());
    REQUIRE(AST::declarations_with_attribute(*bundle, "").empty());
}

TEST_CASE("declarations_with_attribute finds a tagged static method", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public struct Box {\n"
        "    #[http::get: \"/s\"]\n"
        "    public static function hidden() : void {}\n"
        "}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].function != nullptr);
    REQUIRE(hits[0].function->func_name() == "hidden");
    REQUIRE(hits[0].function->is_static_method());
    REQUIRE_FALSE(hits[0].function->has_receiver());
}

TEST_CASE("declarations_with_attribute finds a tagged constructor", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public struct Box {\n"
        "    public int32 $n;\n"
        "    #[http::get]\n"
        "    public constructor() { $this->n = 1; }\n"
        "}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].function != nullptr);
    REQUIRE(hits[0].function->is_constructor());
}

TEST_CASE("declarations_with_attribute finds a tagged enum", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::component]\n"
        "public enum Color {\n"
        "    case red;\n"
        "}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::component");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].kind == AST::AttrSubjectKind::t_type);
    REQUIRE(hits[0].type != nullptr);
    REQUIRE(hits[0].type->type_name() == "Color");
}

TEST_CASE("declarations_with_attribute finds a tagged generic function", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::get]\n"
        "public function handle<T>(T $v) : void {}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].function != nullptr);
    REQUIRE(hits[0].function->is_generic());
}

TEST_CASE("declarations_with_attribute keeps a name payload", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[http::get: users]\n"
        "public function listUsers() : void {}\n");

    const auto hits = AST::declarations_with_attribute(*bundle, "http::get");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].attribute != nullptr);
    REQUIRE(hits[0].attribute->value.has_value());
    REQUIRE(hits[0].attribute->value->kind == AST::AttributeValueKind::t_name);
    REQUIRE(hits[0].attribute->value->text == "users");
}

TEST_CASE("attr_schema_of wants one callable target", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public struct Route {\n"
        "    public function<int32()> $call;\n"
        "}\n");

    AST::TypeDeclNode *route = EchoTests::type_named(
        bundle->modules.find_module("test"), "Route");
    REQUIRE(route != nullptr);

    const AST::AttrSchema schema = AST::attr_schema_of(
        route->value_type(), bundle->collector.core_types);
    REQUIRE(schema.refusal.empty());
    REQUIRE(schema.target_kind == AST::AttrTargetKind::t_callable);
    REQUIRE(schema.payload_indexes.empty());
}

TEST_CASE("attr_schema_of refuses two target fields", "[attributes][attr]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "public struct Route {\n"
        "    public function<int32()> $call;\n"
        "    public function<int32()> $other;\n"
        "}\n");

    AST::TypeDeclNode *route = EchoTests::type_named(
        bundle->modules.find_module("test"), "Route");
    REQUIRE(route != nullptr);

    const AST::AttrSchema schema = AST::attr_schema_of(
        route->value_type(), bundle->collector.core_types);
    REQUIRE(schema.refusal ==
        "attr::of<T> needs exactly one target field, and this struct has more than one");
}

TEST_CASE("attr_schema_of refuses a type that is not a struct", "[attributes][attr]")
{
    const AST::AttrSchema schema = AST::attr_schema_of(
        AST::ValueType(AST::ValueTypePrimitive::t_int32),
        AST::CoreTypes());
    REQUIRE(schema.refusal == "attr::of<T> needs T to be a struct");
}
