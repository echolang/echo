#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTCompleteness.h>
#include <AST/ASTCopy.h>
#include <AST/ASTValueType.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::prim;
using EchoTests::type_named;

// an incomplete type is a fifth ComplexTypeKind, enum's argument read again: a flag on t_struct
// would let every is_struct() site synthesize a constructor and a zero size
TEST_CASE("an extern struct is an incomplete type of its own kind", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("extern struct Handle;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *decl = type_named(m, "Handle");
    REQUIRE(decl != nullptr);

    REQUIRE(decl->kind() == ComplexTypeKind::t_opaque);
    REQUIRE(decl->complex_type().is_opaque_kind());
    REQUIRE(decl->value_type().is_opaque());
    REQUIRE_FALSE(decl->value_type().is_struct());
    REQUIRE(decl->value_type().has_complex_type());
    REQUIRE_FALSE(decl->value_type().has_property_layout());
    REQUIRE_FALSE(decl->complex_type().has_property_layout());
    REQUIRE(decl->synthesized_constructor() == nullptr);
    REQUIRE(decl->node_description() == "extern struct Handle;");
}

TEST_CASE("an incomplete type inside an extern block is the same kind", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern {\n"
        "    struct Resource;\n"
        "    function eco_resource_start(ptr<Resource> $s) : int32;\n"
        "}\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *decl = type_named(m, "Resource");
    REQUIRE(decl != nullptr);
    REQUIRE(decl->kind() == ComplexTypeKind::t_opaque);

    const auto fns = EchoTests::decls_named(m, "eco_resource_start");
    REQUIRE(fns.size() == 1);
    REQUIRE(fns[0]->args.size() == 1);
    REQUIRE(fns[0]->args[0]->type().is_pointer());
    REQUIRE(fns[0]->args[0]->type().pointee().is_opaque());
}

TEST_CASE("two incomplete types are distinct pointees", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern struct Handle;\n"
        "extern struct Resource;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *handle = type_named(m, "Handle");
    auto *resource = type_named(m, "Resource");
    REQUIRE(handle != nullptr);
    REQUIRE(resource != nullptr);

    const ValueType handle_ptr = ValueType::make_pointer(handle->value_type(), true);
    const ValueType resource_ptr = ValueType::make_pointer(resource->value_type(), true);

    REQUIRE(handle_ptr != resource_ptr);
    REQUIRE_FALSE(is_implicitly_convertible(handle_ptr, resource_ptr));
    REQUIRE_FALSE(is_implicitly_convertible(resource_ptr, handle_ptr));
}

TEST_CASE("ptr to an incomplete type is complete, the type itself is not", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("extern struct Handle;\n");
    auto &m = bundle->modules.find_module("test");
    auto *decl = type_named(m, "Handle");
    REQUIRE(decl != nullptr);

    REQUIRE(type_completeness(decl->value_type()) == TypeCompleteness::t_incomplete);
    REQUIRE(type_completeness(ValueType::make_pointer(decl->value_type(), true))
        == TypeCompleteness::t_complete);

    REQUIRE(incomplete_use_refusal(decl->value_type()).has_value());
    REQUIRE_FALSE(incomplete_use_refusal(ValueType::make_pointer(decl->value_type(), true)).has_value());
    REQUIRE(incomplete_use_refusal(ValueType::make_pointer(decl->value_type(), false)).has_value());

    REQUIRE(classify_copy(decl->value_type()) == CopyKind::t_none);
}

TEST_CASE("a ptr to an incomplete type infers as the pointer", "[opaque]")
{
    // AST::read_peels_pointer: `$h` of type `ptr<Handle>` is the address, not a load of Handle.
    // peeling here would refuse `$copy` as a by-value incomplete type
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern struct Handle;\n"
        "ptr<Handle> $h = null;\n"
        "$copy = $h;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    bool found = false;
    for (auto *decl : m.nodes.of_type<VarDeclNode>()) {
        if (decl->name_full() != "$copy") {
            continue;
        }

        found = true;
        REQUIRE(decl->type().is_pointer());
        REQUIRE(decl->type().is_nullable());
        REQUIRE(decl->type().pointee().is_opaque());
    }
    REQUIRE(found);
}

TEST_CASE("a primitive and a pointer stay complete", "[opaque]")
{
    REQUIRE(type_completeness(prim(ValueTypePrimitive::t_int32)) == TypeCompleteness::t_complete);
    REQUIRE(type_completeness(ValueType::make_pointer(prim(ValueTypePrimitive::t_int32), true))
        == TypeCompleteness::t_complete);
}

TEST_CASE("an extern class is refused", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("extern class Widget;\n");
    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "extern class"));
}

TEST_CASE("an extern struct with a body is refused", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("extern struct Foo { int32 $x; }\n");
    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "incomplete and has no body"));
}

TEST_CASE("a by-value incomplete parameter is refused", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern struct Handle;\n"
        "function f(Handle $x) : void {}\n");
    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "incomplete type"));
}

TEST_CASE("mixing two incomplete pointer types is refused at a call", "[opaque]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern struct Handle;\n"
        "extern struct Resource;\n"
        "extern {\n"
        "    function start_handle(ptr<Handle> $h) : int32;\n"
        "}\n"
        "function go(ptr<Resource> $r) : int32 {\n"
        "    return start_handle($r);\n"
        "}\n");
    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "Handle"));
}

TEST_CASE("a member of an incomplete type is one refusal", "[opaque]")
{
    // has_complex_type is true of an opaque name, so looking up `x` would report UnknownMember
    // and then the incomplete-type arm would report again. one sentence, the incomplete one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern struct Handle;\n"
        "ptr<Handle> $h = null;\n"
        "echo $h->x;\n");
    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "incomplete type"));
    REQUIRE(has_issue_containing(*bundle, "has no members"));
    REQUIRE_FALSE(has_issue_containing(*bundle, "has no member named"));
    REQUIRE(bundle->collector.error_count() == 1);
}
