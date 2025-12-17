#include <catch2/catch_test_macros.hpp>

#include <AST/ASTValueType.h>
#include <AST/ASTNodeReference.h>
#include <AST/TypeNode.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"

TEST_CASE( "type inference", "[Integration][Autocast][Numbers]" )
{
    REQUIRE_NODE_DESC(
        "$a = 1;",
        "vardecl<type<int32>>($a) = literal<int32>(1)"
    );

    REQUIRE_NODE_DESC(
        "$a = -1;",
        "vardecl<type<int32>>($a) = literal<int32>(-1)"
    );

    // float
    REQUIRE_NODE_DESC(
        "$a = 1.0;",
        "vardecl<type<float64>>($a) = literal<float64>(1.0)"
    );

    REQUIRE_NODE_DESC(
        "$a = 1.0f;",
        "vardecl<type<float32>>($a) = literal<float32>(1.0f)"
    );

    // hex
    REQUIRE_NODE_DESC(
        "$a = 0xFF;",
        "vardecl<type<uint8>>($a) = literal<uint8>(255 [0xFF])"
    );

    REQUIRE_NODE_DESC(
        "$a = 0xFFFF;",
        "vardecl<type<uint16>>($a) = literal<uint16>(65535 [0xFFFF])"
    );

    REQUIRE_NODE_DESC(
        "$a = 0xFFFFFFFF;",
        "vardecl<type<uint32>>($a) = literal<uint32>(4294967295 [0xFFFFFFFF])"
    );

    REQUIRE_NODE_DESC(
        "$a = 0xFFFFFFFFFFFFFFFF;",
        "vardecl<type<uint64>>($a) = literal<uint64>(18446744073709551615 [0xFFFFFFFFFFFFFFFF])"
    );
}

TEST_CASE( "init autocast (T = int)", "[Integration][Autocast][Numbers]" )
{
    // basic int
    REQUIRE_NODE_DESC(
        "int $a = 1;",
        "vardecl<type<int32>>($a) = literal<int32>(1)"
    );

    // uint
    REQUIRE_NODE_DESC(
        "uint $a = 1;",
        "vardecl<type<uint32>>($a) = literal<uint32>(1)"
    );

    // float
    REQUIRE_NODE_DESC(
        "float $a = 1;",
        "vardecl<type<float32>>($a) = literal<float32>(1.0f [1])"
    );
    REQUIRE_NODE_DESC(
        "float $a = -42;",
        "vardecl<type<float32>>($a) = literal<float32>(-42.0f [-42])"
    );

    // float64
    REQUIRE_NODE_DESC(
        "float64 $a = 1;",
        "vardecl<type<float64>>($a) = literal<float64>(1.0 [1])"
    );

    // bool
    REQUIRE_NODE_DESC(
        "bool $a = 1;",
        "vardecl<type<bool>>($a) = literal<bool>(true [1])"
    );

    REQUIRE_NODE_DESC(
        "bool $a = 0;",
        "vardecl<type<bool>>($a) = literal<bool>(false [0])"
    );
}

TEST_CASE( "init autocast (T = float)", "[Integration][Autocast][Numbers]" )
{
    // int
    REQUIRE_NODE_DESC(
        "int $a = 42.0;",
        "vardecl<type<int32>>($a) = literal<int32>(42 [42.0])"
    );

    REQUIRE_NODE_DESC(
        "int $a = 42.0f;",
        "vardecl<type<int32>>($a) = literal<int32>(42 [42.0f])"
    );

    // uint
    REQUIRE_NODE_DESC(
        "uint $a = 42.0;",
        "vardecl<type<uint32>>($a) = literal<uint32>(42 [42.0])"
    );

    REQUIRE_NODE_DESC(
        "uint $a = 42.0f;",
        "vardecl<type<uint32>>($a) = literal<uint32>(42 [42.0f])"
    );

    // float
    REQUIRE_NODE_DESC(
        "float $a = 42.0f;",
        "vardecl<type<float32>>($a) = literal<float32>(42.0f)"
    );

    REQUIRE_NODE_DESC(
        "float $a = 42.0;",
        "vardecl<type<float32>>($a) = literal<float32>(42.0f [42.0])"
    );

    // float64
    REQUIRE_NODE_DESC(
        "float64 $a = 42.0f;",
        "vardecl<type<float64>>($a) = literal<float64>(42.0 [42.0f])"
    );

    REQUIRE_NODE_DESC(
        "float64 $a = 42.0;",
        "vardecl<type<float64>>($a) = literal<float64>(42.0)"
    );
}


TEST_CASE( "init autocast (T = bool)", "[Integration][Autocast][Numbers]" )
{
    REQUIRE_NODE_DESC(
        "int $a = true;",
        "vardecl<type<int32>>($a) = literal<int32>(1 [true])"
    );

    REQUIRE_NODE_DESC(
        "int $a = false;",
        "vardecl<type<int32>>($a) = literal<int32>(0 [false])"
    );

    REQUIRE_NODE_DESC(
        "uint8 $a = true;",
        "vardecl<type<uint8>>($a) = literal<uint8>(1 [true])"
    );

    REQUIRE_NODE_DESC(
        "uint8 $a = false;",
        "vardecl<type<uint8>>($a) = literal<uint8>(0 [false])"
    );

    // test invalid conversions
    EchoTests::assert_code_emits_issue(
        "float $a = true;",
        "Invalid type conversion: The boolean literal 'true' cannot be implicitly converted to the expected type 'float32'."
    );

    EchoTests::assert_code_emits_issue(
        "float64 $a = false;",
        "Invalid type conversion: The boolean literal 'false' cannot be implicitly converted to the expected type 'float64'."
    );
}

// TEST_CASE( "struct members", "[Integration][Autocast][Numbers]" )
// {
//     REQUIRE_NODE_DESC(
//         "struct A { int $x; }; $a = A(1);"
//         "$b = $a->x * 2.0;",
//         "vardecl<type<int32>>($a) = literal<int32>(1 [true])"
//     );
// }
