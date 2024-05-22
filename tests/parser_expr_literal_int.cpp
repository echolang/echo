#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"

TEST_CASE( "literal int", "[Parser Literal int]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42"
    );

    auto lit = Parser::parse_expr(env.payload);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<int32>(42)");
}

TEST_CASE( "large literal int", "[Parser Literal int]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42424242424242424"
    );

    auto lit = Parser::parse_expr(env.payload);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<int64>(42424242424242424)");
}

TEST_CASE( "expect float literal int", "[Parser Literal int]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_float32));
    auto lit = Parser::parse_expr(env.payload, &expected);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<float32>(42)");
}

TEST_CASE( "expect int8 literal int", "[Parser Literal int]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "42"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_int8));
    auto lit = Parser::parse_expr(env.payload, &expected);

    REQUIRE(env.collector->issues.size() == 0);
    REQUIRE(lit->node_description() == "literal<int8>(42)");
}

TEST_CASE( "expect uint8 literal int would underflow", "[Parser Literal int]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "-42"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_uint8));
    auto lit = Parser::parse_expr(env.payload, &expected);

    REQUIRE(env.collector->issues.size() == 1);
    auto &issue = env.collector->issues[0];
    REQUIRE(issue->severity == AST::IssueSeverity::Error);
    REQUIRE(issue->message().find("is negative") != std::string::npos);
    REQUIRE(lit == nullptr);
}

TEST_CASE( "expect uint8 literal int would overflow", "[Parser Literal int]" ) 
{
    auto env = EchoTests::tests_make_parser_env(
        "256"
    );

    auto expected = AST::TypeNode(AST::ValueType(AST::ValueTypePrimitive::t_uint8));
    auto lit = Parser::parse_expr(env.payload, &expected);

    REQUIRE(env.collector->issues.size() == 1);
    auto &issue = env.collector->issues[0];
    REQUIRE(issue->severity == AST::IssueSeverity::Error);
    REQUIRE(issue->message().find("overflow") != std::string::npos);
    REQUIRE(lit == nullptr);
}