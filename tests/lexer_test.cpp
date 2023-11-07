#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <Lexer.h>

TEST_CASE( "Strings", "[lexer]" ) {
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "'foo', 'bar'");

    REQUIRE( tokens.tokens.size() == 3 );
    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.tokens[1].type == Token::Type::t_comma );
    REQUIRE( tokens.tokens[2].type == Token::Type::t_string_literal );

    // check the values
    REQUIRE( tokens.token_values[0] == "'foo'" );

    tokens.clear();

    // different quotes
    lexer.tokenize(tokens, "\"foo\", \"bar\"");
    REQUIRE( tokens.tokens.size() == 3 );
    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.tokens[1].type == Token::Type::t_comma );
    REQUIRE( tokens.tokens[2].type == Token::Type::t_string_literal );

    // check the values
    REQUIRE( tokens.token_values[0] == "\"foo\"" );

    // test empty string
    tokens.clear();
    lexer.tokenize(tokens, "''");

    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.token_values[0] == "''" );

    // test string with escaped quotes
    tokens.clear();
    lexer.tokenize(tokens, "'\\'foo\\''");

    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.token_values[0] == "'\\'foo\\''" );

    // test line breaks
    tokens.clear();
    lexer.tokenize(tokens, "'foo\nbar'");

    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.token_values[0] == "'foo\nbar'" );
}


TEST_CASE( "HexLiterals", "[lexer]" ) {
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "0x0, 0x1");

    REQUIRE( tokens.tokens.size() == 3 );
    REQUIRE( tokens.tokens[0].type == Token::Type::t_hex_literal );
    REQUIRE( tokens.tokens[1].type == Token::Type::t_comma );
    REQUIRE( tokens.tokens[2].type == Token::Type::t_hex_literal );

    // check the values
    REQUIRE( tokens.token_values[0] == "0x0" );
    REQUIRE( tokens.token_values[2] == "0x1" );

    // check long hex literals
    tokens.clear();
    lexer.tokenize(tokens, "0x1234567890abcdef");
    REQUIRE( tokens.tokens[0].type == Token::Type::t_hex_literal );
    REQUIRE( tokens.token_values[0] == "0x1234567890abcdef" );

    // check uppercase hex literals
    tokens.clear();
    lexer.tokenize(tokens, "0x1234567890ABCDEF");
    REQUIRE( tokens.tokens[0].type == Token::Type::t_hex_literal );
    REQUIRE( tokens.token_values[0] == "0x1234567890ABCDEF" );
}