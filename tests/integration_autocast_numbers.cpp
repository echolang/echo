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

    // **0 and 1 are the two values a bool holds.** anything else is refused - that is not a width
    // question, it is that `3` is not either of those, and tests_eco/errors/literal_at_bool_destination
    // pins the sentence. the override is what get_bool_value reads, so the token `1` is `true`
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


// **the mirror of the arm above, and refused for the same reason.** `int $a = true;` used to become
// `literal<int32>(1 [true])`, which reads as though the language had a number for `true` - it does
// not, and a variable still converts (`int32 $n = $flag;`) exactly as a float still truncates. what
// changed is only what a *written* literal may mean
TEST_CASE( "a bool literal at a numeric destination", "[Integration][Autocast][Numbers]" )
{
    EchoTests::assert_code_emits_issue(
        "int $a = true;",
        "Invalid type conversion: a literal of type 'bool' cannot be written where a 'int32' is expected - Echo has no truthiness in a written literal, so say which of the two you meant"
    );

    EchoTests::assert_code_emits_issue(
        "uint8 $a = false;",
        "Invalid type conversion: a literal of type 'bool' cannot be written where a 'uint8' is expected - Echo has no truthiness in a written literal, so say which of the two you meant"
    );

    EchoTests::assert_code_emits_issue(
        "float $a = true;",
        "Invalid type conversion: a literal of type 'bool' cannot be written where a 'float32' is expected - Echo has no truthiness in a written literal, so say which of the two you meant"
    );

    EchoTests::assert_code_emits_issue(
        "float64 $a = false;",
        "Invalid type conversion: a literal of type 'bool' cannot be written where a 'float64' is expected - Echo has no truthiness in a written literal, so say which of the two you meant"
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
