#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTCopy.h>
#include <AST/ASTDestruction.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ASTValueType.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

namespace
{
    // no `string` anywhere: EchoTests::tests_make_parsed_bundle parses without the standard library,
    // so a core type is unbound and `: string` would be refused. the e2e corpus is where the string
    // backing and a string payload are exercised
    const char *k_curl_error =
        "class Body { int32 $len; }\n"
        "enum CurlError {\n"
        "    case cannot_connect;\n"
        "    case timeout(int32 $after);\n"
        "    case http(int32 $code, Body $body);\n"
        "}\n";
}

// an enum is a fourth ComplexTypeKind *and* a fourth ValueTypeKind, which is the opposite call from
// ComplexType::is_optional one screen away - so what this pins is that the second half landed. a flag
// would have left every is_struct() site answering yes, and the first one to matter synthesizes a
// field-wise constructor that seats a discriminant by hand
TEST_CASE("an enum is a declared type of its own kind", "[enum]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_curl_error);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *decl = type_named(m, "CurlError");
    REQUIRE(decl != nullptr);

    REQUIRE(decl->kind() == ComplexTypeKind::t_enum);
    REQUIRE(decl->complex_type().is_enum_kind());
    REQUIRE(decl->value_type().is_enum());

    // it is not a struct, and that is the whole of the safety argument
    REQUIRE_FALSE(decl->value_type().is_struct());

    // but it does have a layout, which is what lets member access, TBAA and debug info reach it
    REQUIRE(decl->value_type().has_property_layout());
    REQUIRE(decl->value_type().has_complex_type());

    // and no field-wise constructor: `CurlError(2, 30, 404, "...")` would be a door straight past the
    // case table, and it is refused at the declaration rather than left to the private-property rule
    REQUIRE(decl->synthesized_constructor() == nullptr);
}

// the AST layout is **flat** - one property per payload field, all present at once - and that is
// what buys classify_copy and the case table their answers with no arm of their own. LLVM storage
// overlays the cases; that is TypeLowering, and it must not become a `[N x i8]` *property*
TEST_CASE("an enum's layout is the tag then one property per payload field", "[enum]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_curl_error);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const ComplexType &type = type_named(m, "CurlError")->complex_type();

    // the tag first, so an all-zero value is the first case written
    REQUIRE(type.property_count() == 4);
    REQUIRE(type.get_property(k_enum_tag_index).name == k_enum_tag_name);
    REQUIRE(type.get_property_type(k_enum_tag_index) == ValueType(ValueTypePrimitive::t_uint8));

    REQUIRE(type.get_property(1).name == "__c1_after");
    REQUIRE(type.get_property(2).name == "__c2_code");
    REQUIRE(type.get_property(3).name == "__c2_body");

    // a case that carries nothing occupies no property at all
    const ComplexType::EnumCase *none = type.find_enum_case("cannot_connect");
    REQUIRE(none != nullptr);
    REQUIRE(none->ordinal == 0);
    REQUIRE(none->discriminant == 0);
    REQUIRE_FALSE(none->has_payload());

    // and one that does records a contiguous range, which is a fact about how the layout is built
    const ComplexType::EnumCase *http = type.find_enum_case("http");
    REQUIRE(http != nullptr);
    REQUIRE(http->ordinal == 2);
    REQUIRE(http->payload_field_count == 2);
    REQUIRE(http->first_payload_property == 2);
    REQUIRE(http->payload_field_names[0] == "code");
    REQUIRE(http->payload_field_names[1] == "body");
}

// **the flat layout is what makes this answer correctly with no enum arm in ASTCopy.cpp at all**, and
// it is the thing a future overlapped-union layout would break silently: a `[N x i8]` payload folds to
// t_bytes, so an enum owning a string would be copied by memcpy and freed twice
TEST_CASE("an enum owning a payload is synthesizable, and one owning nothing copies as bytes", "[enum]")
{
    // a class is owning without needing the standard library, which this fixture does not load
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Held { int32 $v; }\n"
        "enum Owning { case none; case boxed(Held $h); }\n"
        "enum Plain { case left; case right; }\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(classify_copy(type_named(m, "Owning")->value_type()) == CopyKind::t_synthesizable);
    REQUIRE(needs_destruction(type_named(m, "Owning")->value_type()));

    REQUIRE(classify_copy(type_named(m, "Plain")->value_type()) == CopyKind::t_bytes);
    REQUIRE_FALSE(needs_destruction(type_named(m, "Plain")->value_type()));
}

// a case is a static function and nothing else, which is what notes/statics.md predicted before enums
// existed: find_static_functions deliberately never asks what kind the owner is
TEST_CASE("every case is a static function on the enum", "[enum]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_curl_error);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const ComplexType &type = type_named(m, "CurlError")->complex_type();

    REQUIRE(type.static_methods().size() == 3);

    auto timeouts = find_static_functions(&type, "timeout");
    REQUIRE(timeouts.size() == 1);
    REQUIRE(timeouts[0]->member_kind == MemberKind::t_static_method);
    REQUIRE_FALSE(timeouts[0]->has_receiver());
    REQUIRE(timeouts[0]->args.size() == 1);

    // a case of no payload is a static of no arguments, which is what makes the paren-free
    // `CurlError::cannot_connect` the call it is rather than a form of its own
    auto refused = find_static_functions(&type, "cannot_connect");
    REQUIRE(refused.size() == 1);
    REQUIRE(refused[0]->args.empty());
}

// the backing type is read out of the conformance clause, which is one grammar rather than two - so an
// enum may be backed and conform in the same list, told apart by what each entry names
TEST_CASE("a backed enum records its backing and takes the tag's type from it", "[enum]")
{
    // the integer backing only: `string` is a core type and this fixture loads no standard library.
    // tests_eco/enums/backed_values covers the string one, where there is a `string` to bind
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "enum Status : int32 { case ok = 200; case not_found = 404; }\n"
        "enum Plain { case left; case right; }\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // an integer backing *is* the discriminant, so `not_found` is tag 404 and not tag 1
    const ComplexType &status = type_named(m, "Status")->complex_type();
    REQUIRE(status.enum_backing.has_value());
    REQUIRE(status.get_property_type(k_enum_tag_index) == ValueType(ValueTypePrimitive::t_int32));
    REQUIRE(status.find_enum_case("ok")->discriminant == 200);
    REQUIRE(status.find_enum_case("not_found")->discriminant == 404);

    // a backed enum gets `value()`, a function rather than a property - and an unbacked one does not,
    // there being no value to answer with
    REQUIRE(find_member_functions(&status, "value").size() == 1);

    const ComplexType &plain = type_named(m, "Plain")->complex_type();
    REQUIRE_FALSE(plain.enum_backing.has_value());
    REQUIRE(plain.get_property_type(k_enum_tag_index) == ValueType(ValueTypePrimitive::t_uint8));
    REQUIRE(find_member_functions(&plain, "value").empty());
}

// the refusals are what keep the layout the compiler's. each is reported at the member rather than on
// the way past, so an enum never carries a property or a constructor a later pass could believe in
TEST_CASE("an enum body refuses the member shapes that would reach past its cases", "[enum]")
{
    SECTION("a property") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "enum E { case a; public int32 $extra; }\n");
        REQUIRE(has_issue_containing(*bundle, "is an enum, so it cannot declare a property"));
    }

    SECTION("a constructor") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "enum E { case a; constructor() {} }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot declare a constructor"));
    }

    SECTION("a constant, because E::name already denotes a case") {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "enum E { case a; const LIMIT = 4; }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot declare a constant"));
    }

    SECTION("a case outside an enum") {
        auto bundle = EchoTests::tests_make_parsed_bundle("struct P { case nope; }\n");
        REQUIRE(has_issue_containing(*bundle, "only an `enum` has them"));
    }

    SECTION("a backing type that cannot be one") {
        auto bundle = EchoTests::tests_make_parsed_bundle("enum E : float64 { case a = 1; }\n");
        REQUIRE(has_issue_containing(*bundle, "cannot back an enum"));
    }

    SECTION("two cases of one name") {
        auto bundle = EchoTests::tests_make_parsed_bundle("enum E { case a; case a; }\n");
        REQUIRE(has_issue_containing(*bundle, "already declares a case named"));
    }
}

// an instantiation has a ComplexType and no declaration node, which is the whole reason the case table
// lives on the layout rather than on the declaration. without the carry-across in
// TypeRegistry::get_or_create_instantiation, a `match` over a generic enum finds no cases and reads as
// non-exhaustive against a set of nothing - so the program below not being refused *is* the assertion.
// tests_eco/enums/generic_enum runs it and checks what it answers
TEST_CASE("a generic enum instantiates, and its cases survive the instantiation", "[enum][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "enum Slot<T> { case empty; case filled(T $value); }\n"
        "function unwrap_or(Slot<int32> $s, int32 $fallback) : int32 {\n"
        "    return match ($s) {\n"
        "        Slot<int32>::empty      => $fallback,\n"
        "        Slot<int32>::filled($v) => $v,\n"
        "    };\n"
        "}\n"
        "echo unwrap_or(Slot<int32>::filled(9), 0);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    const ComplexType &tmpl = type_named(m, "Slot")->complex_type();

    REQUIRE(tmpl.is_enum_kind());
    REQUIRE(tmpl.enum_cases().size() == 2);
    REQUIRE(tmpl.find_enum_case("filled")->payload_field_count == 1);

    // and the case constructors inherit the owner's parameters through the one owner of that shape -
    // without it a case of *no* arguments has nothing to infer `T` from and no seed saying the owner
    // may supply it, which is what `Slot<int32>::empty` needs
    auto empties = find_static_functions(&tmpl, "empty");
    REQUIRE(empties.size() == 1);
    REQUIRE(empties[0]->is_generic());
}
