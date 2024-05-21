#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"

TEST_CASE( "literal double", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "3.14"
    );

    auto lit = Parser::parse_expr(env.payload);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<float64>(3.14)");
}

TEST_CASE( "literal float", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "3.14f"
    );

    auto lit = Parser::parse_expr(env.payload);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<float32>(3.14f)");
}

TEST_CASE( "expect float but got double", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42.0"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_float32));
    auto lit = Parser::parse_expr(env.payload, &expected);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<float32>(42.0)");
}

TEST_CASE( "expect float but got double, precision loss", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "123456.123456"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_float32));
    auto lit = Parser::parse_expr(env.payload, &expected);

    // ensure we collected a warning
    REQUIRE(env.collector->issues.size() == 1);
    auto &warning = env.collector->issues[0];
    REQUIRE(warning->severity == AST::IssueSeverity::Warning);
    REQUIRE(warning->message().contains("loss of precision"));

    REQUIRE(lit->node_description() == "literal<float32>(123456.125000)");
}

TEST_CASE( "expect double but got float", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42.0f"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_float64));
    auto lit = Parser::parse_expr(env.payload, &expected);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<float64>(42.0f)");
}

TEST_CASE( "expect int8 but got float", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42.0f"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_int8));
    auto ref = Parser::parse_expr_ref(env.payload, &expected);
    auto lit = ref.get_ptr<AST::LiteralIntExprNode>();

    REQUIRE(env.collector->issues.size() == 0);
    
    REQUIRE(lit->node_description() == "literal<int8>(42)");
    REQUIRE(lit->int8_value() == 42);
}

TEST_CASE( "expect int8 but got uncastable float", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42.85f"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_int8));
    auto ref = Parser::parse_expr_ref(env.payload, &expected);

    // ensure we collected a warning
    REQUIRE(env.collector->issues.size() == 1);
    auto &warning = env.collector->issues[0];
    REQUIRE(warning->severity == AST::IssueSeverity::Error);
    REQUIRE(warning->message().contains("cannot be implicitly converted"));

    // there should be no reference
    REQUIRE_FALSE(ref.has());
}

TEST_CASE( "expect int8 but got float that would overflow", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "128.0f"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_int8));
    auto ref = Parser::parse_expr_ref(env.payload, &expected);

    // ensure we collected a warning
    REQUIRE(env.collector->issues.size() == 1);
    auto &warning = env.collector->issues[0];
    REQUIRE(warning->severity == AST::IssueSeverity::Error);
    REQUIRE(warning->message().contains("overflow"));

    // there should be no reference
    REQUIRE_FALSE(ref.has());
}

TEST_CASE( "expect uint8 but got float that would underflow", "[Parser Literal float]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "-1.0f"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_uint8));
    auto ref = Parser::parse_expr_ref(env.payload, &expected);

    // ensure we collected a warning
    REQUIRE(env.collector->issues.size() == 1);
    auto &warning = env.collector->issues[0];
    REQUIRE(warning->severity == AST::IssueSeverity::Error);
    REQUIRE(warning->message().contains("underflow"));

    // there should be no reference
    REQUIRE_FALSE(ref.has());
}