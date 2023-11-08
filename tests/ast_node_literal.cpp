#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/LiteralValueNode.h>

TEST_CASE( "test description", "[AST Literal Value]" ) 
{
    auto tokens = TokenCollection();
    tokens.push("123", Token::Type::t_integer_literal, 0, 0);
    tokens.push("123.456", Token::Type::t_floating_literal, 0, 0);
    tokens.push("true", Token::Type::t_bool_literal, 0, 0);
    tokens.push("false", Token::Type::t_bool_literal, 0, 0);
    tokens.push("0x123", Token::Type::t_hex_literal, 0, 0);
    tokens.push("0b101", Token::Type::t_binary_literal, 0, 0);
    tokens.push("'foo'", Token::Type::t_string_literal, 0, 0);

    auto int_node = AST::LiteralValueNode(tokens[0]);
    REQUIRE( int_node.node_description() == "literal<int>(123)" );

    auto float_node = AST::LiteralValueNode(tokens[1]);
    REQUIRE( float_node.node_description() == "literal<float>(123.456)" );

    auto true_node = AST::LiteralValueNode(tokens[2]);
    REQUIRE( true_node.node_description() == "literal<bool>(true)" );

    auto false_node = AST::LiteralValueNode(tokens[3]);
    REQUIRE( false_node.node_description() == "literal<bool>(false)" );

    auto hex_node = AST::LiteralValueNode(tokens[4]);
    REQUIRE( hex_node.node_description() == "literal<hex>(0x123)" );

    auto binary_node = AST::LiteralValueNode(tokens[5]);
    REQUIRE( binary_node.node_description() == "literal<binary>(0b101)" );

    auto string_node = AST::LiteralValueNode(tokens[6]);
    REQUIRE( string_node.node_description() == "literal<string>('foo')" );
}