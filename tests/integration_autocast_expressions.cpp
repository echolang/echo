#include <catch2/catch_test_macros.hpp>

#include <AST/ASTValueType.h>
#include <AST/ASTNodeReference.h>
#include <AST/TypeNode.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"

TEST_CASE( "implicit casting rules", "[Integration][Autocast][Expressions]" )
{
    // auto cast to float
    REQUIRE_NODE_DESC(
        "$a = 2 * 3.14f + 10;",
        "vardecl<type<float32>>($a) = "
        "binexp<float32>(binexp<float32>(literal<float32>(2.0f [2]) * literal<float32>(3.14f)) + literal<float32>(10.0f [10]))"
    );

    // auto cast to double
    REQUIRE_NODE_DESC(
        "$a = 2 * 3.14 + 10;",
        "vardecl<type<float64>>($a) = "
        "binexp<float64>(binexp<float64>(literal<float64>(2.0 [2]) * literal<float64>(3.14)) + literal<float64>(10.0 [10]))"
    );

    // forced float
    REQUIRE_NODE_DESC(
        "float $a = 2 * 3.14 + 10;",
        "vardecl<type<float32>>($a) = "
        "binexp<float32>(binexp<float32>(literal<float32>(2.0f [2]) * literal<float32>(3.14f [3.14])) + literal<float32>(10.0f [10]))"
    );

    // runtime cast
    REQUIRE_NODE_DESC(
        "$a = 5; $b = $a * 3.14;",
        "vardecl<type<int32>>($a) = literal<int32>(5)\n"
        "vardecl<type<float64>>($b) = binexp<float64>(cast<float64>(varref<int32>(var($a))) * literal<float64>(3.14))"
    );

    // runtime cast with explicit type
    REQUIRE_NODE_DESC(
        "$a = 5; float $b = $a * 3.14;",
        "vardecl<type<int32>>($a) = literal<int32>(5)\n"
        "vardecl<type<float32>>($b) = binexp<float32>(cast<float32>(varref<int32>(var($a))) * literal<float32>(3.14f [3.14]))"
    );

    // auto cast to larger float
    REQUIRE_NODE_DESC(
        "$a = 2.0f + 3.14;",
        "vardecl<type<float64>>($a) = binexp<float64>(literal<float64>(2.0 [2.0f]) + literal<float64>(3.14))"
    );

    // auto cast smaller int to larger int
    REQUIRE_NODE_DESC(
        "$a = 5; int8 $b = 2; $c = $a + $b;",
        "vardecl<type<int32>>($a) = literal<int32>(5)\n"
        "vardecl<type<int8>>($b) = literal<int8>(2)\n"
        "vardecl<type<int32>>($c) = binexp<int32>(varref<int32>(var($a)) + cast<int32>(varref<int8>(var($b))))"
    );
}
