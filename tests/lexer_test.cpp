#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <Lexer.h>

TEST_CASE( "Token References", "[lexer]" ) {

    TokenCollection tokens;

    tokens.push("foo", Token::Type::t_identifier, 0, 0);
    tokens.push("bar", Token::Type::t_identifier, 0, 0);

    auto foo = tokens[0];
    auto bar = tokens[1];

    REQUIRE( foo.value() == "foo" );
    REQUIRE( bar.value() == "bar" );

    REQUIRE( foo.type() == Token::Type::t_identifier );
    REQUIRE( bar.type() == Token::Type::t_identifier );
}

TEST_CASE( "Numeric Literals", "[lexer]" ) 
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(
        tokens, 
        "42 " // decimal
        "-42 " // negative decimal

        "1.0 " // double
        "-1.0 " // negative double

        "1.0f " // float
        "-1.0f " // negative float

        "1. " // double autozero
        "-1. " // negative double autozero

        "1.f " // float autozero
        "-1.f " // negative float autozero

        "3.1415926535897932384626433" // pi

        // todo add scientific notation
    );

    auto decimal = tokens[0];
    REQUIRE( decimal.type() == Token::Type::t_integer_literal );
    REQUIRE( decimal.value() == "42" );

    auto negative_decimal = tokens[1];
    REQUIRE( negative_decimal.type() == Token::Type::t_integer_literal );
    REQUIRE( negative_decimal.value() == "-42" );

    auto double_literal = tokens[2];
    REQUIRE( double_literal.type() == Token::Type::t_floating_literal );
    REQUIRE( double_literal.value() == "1.0" );

    auto negative_double_literal = tokens[3];
    REQUIRE( negative_double_literal.type() == Token::Type::t_floating_literal );
    REQUIRE( negative_double_literal.value() == "-1.0" );

    auto float_literal = tokens[4];
    REQUIRE( float_literal.type() == Token::Type::t_floating_literal );
    REQUIRE( float_literal.value() == "1.0f" );

    auto negative_float_literal = tokens[5];
    REQUIRE( negative_float_literal.type() == Token::Type::t_floating_literal );
    REQUIRE( negative_float_literal.value() == "-1.0f" );

    auto double_autozero = tokens[6];
    REQUIRE( double_autozero.type() == Token::Type::t_floating_literal );
    REQUIRE( double_autozero.value() == "1.0" );

    auto negative_double_autozero = tokens[7];
    REQUIRE( negative_double_autozero.type() == Token::Type::t_floating_literal );
    REQUIRE( negative_double_autozero.value() == "-1.0" );

    auto float_autozero = tokens[8];
    REQUIRE( float_autozero.type() == Token::Type::t_floating_literal );
    REQUIRE( float_autozero.value() == "1.0f" );

    auto negative_float_autozero = tokens[9];
    REQUIRE( negative_float_autozero.type() == Token::Type::t_floating_literal );
    REQUIRE( negative_float_autozero.value() == "-1.0f" );

    auto pi = tokens[10];
    REQUIRE( pi.type() == Token::Type::t_floating_literal );
    REQUIRE( pi.value() == "3.1415926535897932384626433" );
}

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

TEST_CASE( "Unterminated String", "[lexer]" ) {
    Lexer lexer;
    TokenCollection tokens;

    REQUIRE_THROWS_AS( lexer.tokenize(tokens, "'foo"), Lexer::UnterminatedStringException );
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

// // tests trying to parse custom operator symbols
// TEST_CASE( "Operator Prepass", "[lexer]" ) {
//     Lexer lexer;
//     TokenCollection tokens;
//     AST::OperatorRegistry ops;

//     std::string code = 
//         "operator (int $foo) <=> (int $bar) : int {"
//         "    return $foo + $bar;"
//         "}"
//         "echo 42 <=> 69";

//     lexer.tokenize_prepass_operators(code, ops);
//     lexer.tokenize(tokens, code, &ops);

//     REQUIRE( tokens.tokens.size() == 17 );

// }