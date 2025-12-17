#include <catch2/catch_test_macros.hpp>

#include <AST/ASTValueType.h>
#include <AST/ASTNodeReference.h>
#include <AST/TypeNode.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"

TEST_CASE( "math operations", "[Integration][Expr][OpOrder]" )
{
    // * > +
    REQUIRE_NODE_DESC_EXPR(
        "69 + 42 * 2;",
        "binexp<int32>(literal<int32>(69) + binexp<int32>(literal<int32>(42) * literal<int32>(2)))"
    );

    // / > -
    REQUIRE_NODE_DESC_EXPR(
        "42 - 6 / 9;",
        "binexp<int32>(literal<int32>(42) - binexp<int32>(literal<int32>(6) / literal<int32>(9)))"
    );

    // / > +
    REQUIRE_NODE_DESC_EXPR(
        "42 + 6 / 9;",
        "binexp<int32>(literal<int32>(42) + binexp<int32>(literal<int32>(6) / literal<int32>(9)))"
    );
    REQUIRE_NODE_DESC_EXPR(
        "42 / 6 + 9;",
        "binexp<int32>(binexp<int32>(literal<int32>(42) / literal<int32>(6)) + literal<int32>(9))"
    );

    // % > +
    REQUIRE_NODE_DESC_EXPR(
        "7 + 10 % 3;",
        "binexp<int32>(literal<int32>(7) + binexp<int32>(literal<int32>(10) % literal<int32>(3)))"
    );

    // ** has highest precedence
    REQUIRE_NODE_DESC_EXPR(
        "2 * 3 ** 2;",
        "binexp<int32>(literal<int32>(2) * binexp<int32>(literal<int32>(3) ** literal<int32>(2)))"
    );
    REQUIRE_NODE_DESC_EXPR(
        "1 + 2 ** 3;",
        "binexp<int32>(literal<int32>(1) + binexp<int32>(literal<int32>(2) ** literal<int32>(3)))"
    );

    // left -> right associativity for +/-
    REQUIRE_NODE_DESC_EXPR(
        "10 - 5 + 2;",
        "binexp<int32>(binexp<int32>(literal<int32>(10) - literal<int32>(5)) + literal<int32>(2))"
    );

    // left -> right associativity for */%
    REQUIRE_NODE_DESC_EXPR(
        "20 / 4 * 3;",
        "binexp<int32>(binexp<int32>(literal<int32>(20) / literal<int32>(4)) * literal<int32>(3))"
    );

    // multiple precedence levels
    REQUIRE_NODE_DESC_EXPR(
        "1 + 2 * 3 - 4;",
        "binexp<int32>(binexp<int32>(literal<int32>(1) + binexp<int32>(literal<int32>(2) * literal<int32>(3))) - literal<int32>(4))"
    );

    // parentheses
    REQUIRE_NODE_DESC_EXPR(
        "(1 + 2) * 3;",
        "binexp<int32>(binexp<int32>(literal<int32>(1) + literal<int32>(2)) * literal<int32>(3))"
    );

    // nested parentheses
    REQUIRE_NODE_DESC_EXPR(
        "((2 + 3) * 4) / 5;",
        "binexp<int32>(binexp<int32>(binexp<int32>(literal<int32>(2) + literal<int32>(3)) * literal<int32>(4)) / literal<int32>(5))"
    );

    // bitwise shifts < others
    REQUIRE_NODE_DESC_EXPR(
        "4 + 1 << 2;",
        "binexp<int32>(binexp<int32>(literal<int32>(4) + literal<int32>(1)) << literal<int32>(2))"
    );
    REQUIRE_NODE_DESC_EXPR(
        "16 >> 1 + 1;",
        "binexp<int32>(literal<int32>(16) >> binexp<int32>(literal<int32>(1) + literal<int32>(1)))"
    );
}

TEST_CASE( "prefix unary negation", "[Integration][Expr][OpOrder]" )
{
    // prefix '-' on a parenthesized subexpression wraps the whole group
    REQUIRE_NODE_DESC_EXPR(
        "-(2 + 1);",
        "unexp(-binexp<int32>(literal<int32>(2) + literal<int32>(1)))"
    );

    // a prefix '-' in operand position after a binary operator is unary, not a
    // second binary operator with a missing left-hand side
    REQUIRE_NODE_DESC_EXPR(
        "3 * -(1 + 1);",
        "binexp<int32>(literal<int32>(3) * unexp(-binexp<int32>(literal<int32>(1) + literal<int32>(1))))"
    );
}
