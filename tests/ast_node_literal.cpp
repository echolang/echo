#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"

#define TEST_ASSERT_LIT_DESC(index, type, desc) \
    { \
        auto &lit = tm->nodes.emplace_back<type>(tm->tokens[index]); \
        REQUIRE(lit.node_description() == desc); \
    }

TEST_CASE( "test description", "[AST Literal Value]" ) 
{
    auto tm = EchoTests::tests_make_module_with_content(
        "42 " // int literal
        "-42 " // int literal
        "3.14 " // double literal
        "-3.14 " // double literal
        "3.14f " // float literal
        "-3.14f " // float literal
        "true " // bool literal
        "false " // bool literal
    );

    TEST_ASSERT_LIT_DESC(0, AST::LiteralIntExprNode, "literal<int32>(42)");
    TEST_ASSERT_LIT_DESC(1, AST::LiteralIntExprNode, "literal<int32>(-42)");
    TEST_ASSERT_LIT_DESC(2, AST::LiteralFloatExprNode, "literal<float64>(3.14)");
    TEST_ASSERT_LIT_DESC(3, AST::LiteralFloatExprNode, "literal<float64>(-3.14)");
    TEST_ASSERT_LIT_DESC(4, AST::LiteralFloatExprNode, "literal<float32>(3.14f)");
    TEST_ASSERT_LIT_DESC(5, AST::LiteralFloatExprNode, "literal<float32>(-3.14f)");
    TEST_ASSERT_LIT_DESC(6, AST::LiteralBoolExprNode, "literal<bool>(true)");
    TEST_ASSERT_LIT_DESC(7, AST::LiteralBoolExprNode, "literal<bool>(false)");
}