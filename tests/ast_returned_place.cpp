#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTReturnedPlace.h>
#include <AST/ASTValueType.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include <string>
#include <vector>

#include "helpers.h"

using namespace AST;

using EchoTests::decls_named;
using EchoTests::type_named;

namespace
{
    FunctionDeclNode *method_named(Module &m, const char *name)
    {
        auto decls = decls_named(m, name);

        REQUIRE(decls.size() >= 1);
        return decls[0];
    }
}

TEST_CASE("borrow_result_of answers whole, field, or none", "[AST][ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Probe {\n"
        "    int32 $n;\n"
        "    int32 $m;\n"
        "    function self() : Probe& { return $this; }\n"
        "    function item() : int32& { return $this->n; }\n"
        "    function pick(bool $left) : int32& {\n"
        "        if ($left) {\n"
        "            return $this->n;\n"
        "        } else {\n"
        "            return $this->m;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "interface Unwrappable {\n"
        "    function unwrap() : int32&;\n"
        "}\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    const BorrowResult whole = borrow_result_of(method_named(m, "self"));
    REQUIRE(whole.kind == BorrowResultKind::t_whole);
    REQUIRE(whole.path.empty());

    const BorrowResult field = borrow_result_of(method_named(m, "item"));
    REQUIRE(field.kind == BorrowResultKind::t_field);
    REQUIRE(field.path == std::vector<std::string>{"n"});

    const BorrowResult mixed = borrow_result_of(method_named(m, "pick"));
    REQUIRE(mixed.kind == BorrowResultKind::t_none);

    const BorrowResult iface = borrow_result_of(method_named(m, "unwrap"));
    REQUIRE(iface.kind == BorrowResultKind::t_none);
}

TEST_CASE("borrow_result_of follows a match payload binding onto $this", "[AST][ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "enum Box {\n"
        "    case full(int32 $n);\n"
        "    function take() : int32& {\n"
        "        return match ($this) {\n"
        "            Box::full($n) => $n,\n"
        "        };\n"
        "    }\n"
        "}\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const BorrowResult named = borrow_result_of(method_named(m, "take"));

    REQUIRE(named.kind == BorrowResultKind::t_field);
    REQUIRE(named.path == std::vector<std::string>{enum_payload_property_name(0, "n")});
}

TEST_CASE("remainder_needs_destruction is silent for a whole value and for an optional payload", "[AST][ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Drop { destructor() {} }\n"
        "struct Pair { Drop $a; Drop $b; }\n"
        "Drop? $opt = Drop();\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const ValueType pair = type_named(m, "Pair")->value_type();
    const ValueType drop = type_named(m, "Drop")->value_type();

    REQUIRE_FALSE(remainder_needs_destruction(pair, {}));
    REQUIRE(remainder_needs_destruction(pair, {"a"}));
    REQUIRE(remainder_needs_destruction(pair, {"b"}));

    const ValueType optional = bundle->collector.type_registry.get_or_create_optional(drop);
    REQUIRE_FALSE(remainder_needs_destruction(optional, {k_optional_value_name}));
    REQUIRE(remainder_needs_destruction(optional, {k_optional_has_name}));
}

TEST_CASE("remainder_needs_destruction of an enum overlay counts only the live case", "[AST][ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Drop { destructor() {} }\n"
        "enum Box {\n"
        "    case full(Drop $keep, Drop $out);\n"
        "    case empty;\n"
        "}\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const ValueType box = type_named(m, "Box")->value_type();
    const std::string keep = enum_payload_property_name(0, "keep");
    const std::string out = enum_payload_property_name(0, "out");

    REQUIRE(remainder_needs_destruction(box, {keep}));
    REQUIRE(remainder_needs_destruction(box, {out}));
}

TEST_CASE("remainder_needs_destruction of a class or a user destructor is closed", "[AST][ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Box { int32 $n; function take() : int32& { return $this->n; } }\n"
        "interface Face { function take() : int32&; }\n"
        "struct Holder { int32 $n; destructor() {} function take() : int32& { return $this->n; } }\n"
        "struct Plain { int32 $a; int32 $b; }\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const ValueType box = type_named(m, "Box")->value_type();
    const ValueType face = type_named(m, "Face")->value_type();
    const ValueType holder = type_named(m, "Holder")->value_type();
    const ValueType plain = type_named(m, "Plain")->value_type();

    REQUIRE(remainder_needs_destruction(box, {"n"}));
    REQUIRE(remainder_needs_destruction(face, {"n"}));
    REQUIRE(remainder_needs_destruction(holder, {"n"}));
    REQUIRE_FALSE(remainder_needs_destruction(plain, {"a"}));
    REQUIRE_FALSE(remainder_needs_destruction(plain, {"b"}));
}

TEST_CASE("borrow_result_of does not follow a by-value copy of $this", "[AST][ownership]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Probe {\n"
        "    int32 $n;\n"
        "    function item() : int32& {\n"
        "        Probe $copy = $this;\n"
        "        return $copy->n;\n"
        "    }\n"
        "}\n");

    auto &m = bundle->modules.find_module("test");
    const BorrowResult named = borrow_result_of(method_named(m, "item"));

    REQUIRE(named.kind == BorrowResultKind::t_none);
}
