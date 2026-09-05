#include <catch2/catch_test_macros.hpp>

#include <AST/ASTTypeParam.h>
#include <AST/ASTValueType.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

TEST_CASE("A const value is equal only to the same bits of the same primitive", "[types][generics]")
{
    const ValueType four = ValueType::make_const_value(ValueTypePrimitive::t_usize, 4);
    const ValueType also_four = ValueType::make_const_value(ValueTypePrimitive::t_usize, 4);
    const ValueType five = ValueType::make_const_value(ValueTypePrimitive::t_usize, 5);
    const ValueType four_i32 = ValueType::make_const_value(ValueTypePrimitive::t_int32, 4);

    REQUIRE(four == also_four);
    REQUIRE_FALSE(four == five);
    REQUIRE_FALSE(four == four_i32);
    REQUIRE(four.get_type_desciption() == "4");
    REQUIRE(four.get_mangled_name() != five.get_mangled_name());
}

TEST_CASE("An inline array is identified by element and length", "[types][generics]")
{
    const ValueType int32 = EchoTests::prim(ValueTypePrimitive::t_int32);
    const ValueType four = ValueType::make_const_value(ValueTypePrimitive::t_usize, 4);
    const ValueType five = ValueType::make_const_value(ValueTypePrimitive::t_usize, 5);

    const ValueType a = ValueType::make_inline_array(int32, four);
    const ValueType b = ValueType::make_inline_array(int32, four);
    const ValueType c = ValueType::make_inline_array(int32, five);

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE(a.get_type_desciption() == "int32[4]");
    REQUIRE(a.bound_array_length() == 4);
    REQUIRE(c.bound_array_length() == 5);
}

TEST_CASE("sized<N> intern identity depends on N", "[types][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct sized<const usize N>\n"
        "{\n"
        "    const function n() : usize { return N; }\n"
        "}\n"
        "\n"
        "sized<4> $a;\n"
        "sized<8> $b;\n");

    auto &module = bundle->modules.find_module("test");
    ComplexType *four = nullptr;
    ComplexType *eight = nullptr;

    for (auto *ct : bundle->collector.type_registry.instantiations()) {
        if (!ct->is_instantiated() || !ct->name.has_value()) {
            continue;
        }

        if (*ct->name == "sized<4>") {
            four = ct;
        }

        if (*ct->name == "sized<8>") {
            eight = ct;
        }
    }

    REQUIRE(four != nullptr);
    REQUIRE(eight != nullptr);
    REQUIRE(four != eight);

    REQUIRE(four->instantiation_args.size() == 1);
    REQUIRE(four->instantiation_args[0].is_const_value());
    REQUIRE(four->instantiation_args[0].const_value_bits() == 4);
    REQUIRE(eight->instantiation_args[0].const_value_bits() == 8);

    (void)module;
}

TEST_CASE("A value parameter refuses bits that do not fit its integer type", "[types][generics]")
{
    TypeParamRegistry params;
    TypeParamDecl *n = params.declare("N", 0);
    n->param_kind = TypeParamKind::t_value;
    n->value_type = ValueType(ValueTypePrimitive::t_uint8);

    REQUIRE(n->allows(ValueType::make_const_value(ValueTypePrimitive::t_uint8, 255)));
    REQUIRE(n->allows(ValueType::make_const_value(ValueTypePrimitive::t_int32, 4)));
    REQUIRE_FALSE(n->allows(ValueType::make_const_value(ValueTypePrimitive::t_uint8, 256)));
    REQUIRE_FALSE(n->allows(ValueType::make_const_value(ValueTypePrimitive::t_int32, 300)));
}

TEST_CASE("A type parameter refuses a value parameter", "[types][generics]")
{
    TypeParamRegistry params;
    TypeParamDecl *t = params.declare("T", 0);
    TypeParamDecl *n = params.declare("N", 1);
    n->param_kind = TypeParamKind::t_value;
    n->value_type = ValueType(ValueTypePrimitive::t_usize);

    REQUIRE_FALSE(t->allows(ValueType::make_type_param(n)));
    REQUIRE_FALSE(t->allows(ValueType::make_const_value(ValueTypePrimitive::t_int32, 4)));
    REQUIRE(n->allows(ValueType::make_type_param(n)));
}
