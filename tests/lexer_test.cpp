#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <Lexer.h>

// TEST_CASE( "test matching signature", "[lexer]" ) 
// {
//     REQUIRE( true );
    
//     try {
//         throw Lexer::UnterminatedStringException("snippet", 1, 1);
//     } catch (const Lexer::TokenException &e) {
//         REQUIRE( std::string(e.what()) == "Unterminated string at line 1 offset 1 near: snippet" );
//     }

//     try {
//         throw Lexer::UnterminatedStringException("snippet", 1, 1);
//     } catch (const Lexer::UnterminatedStringException &e) {
//         REQUIRE( std::string(e.what()) == "Unterminated string at line 1 offset 1 near: snippet" );
//         REQUIRE( e.get_snippet() == "snippet" );
//     }
// }


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

    // test utf-8
    tokens.clear();
    lexer.tokenize(tokens, "'🍕'");

    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.token_values[0] == "'🍕'" );
}

TEST_CASE( "Unterminated String", "[lexer]" ) {
    Lexer lexer;
    TokenCollection tokens;

    REQUIRE_THROWS_AS( lexer.tokenize(tokens, "'foo"), Lexer::UnterminatedStringException );
}

TEST_CASE( "Variable Names", "[lexer]" ) 
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "$foo, $bar");

    REQUIRE( tokens.tokens[0].type == Token::Type::t_varname );
    REQUIRE( tokens.tokens[1].type == Token::Type::t_comma );
    REQUIRE( tokens.tokens[2].type == Token::Type::t_varname );

    // check the values
    REQUIRE( tokens.token_values[0] == "$foo" );
    REQUIRE( tokens.token_values[2] == "$bar" );
}

TEST_CASE( "Variable Names Incomplete", "[lexer]" ) 
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "$$bar");

    REQUIRE( tokens.tokens[0].type == Token::Type::t_varname );
    REQUIRE( tokens.tokens[1].type == Token::Type::t_varname );

    // check the values
    REQUIRE( tokens.token_values[0] == "$" );
    REQUIRE( tokens.token_values[1] == "$bar" );
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

TEST_CASE( "Single Line Comments", "[lexer]" ) {
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "foo // bar");

    REQUIRE( tokens.tokens.size() == 1 );
    REQUIRE( tokens.tokens[0].type == Token::Type::t_identifier );

    // check the values
    REQUIRE( tokens.token_values[0] == "foo" );
}

TEST_CASE("Multi Line Comments", "[lexer]") {
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "hey /* this\nis\na\ncomment */ ronon");

    REQUIRE( tokens.tokens.size() == 2 );
    REQUIRE( tokens.tokens[0].type == Token::Type::t_identifier );
    REQUIRE( tokens.tokens[1].type == Token::Type::t_identifier );

    // check the values
    REQUIRE( tokens.token_values[0] == "hey" );
    REQUIRE( tokens.token_values[1] == "ronon" );
}

TEST_CASE("Multi Line Comments Unterminated", "[lexer]") {
    Lexer lexer;
    TokenCollection tokens;

    REQUIRE_THROWS_AS( lexer.tokenize(tokens, "hey /* this\nis\na\ncomment ronon"), Lexer::UnterminatedCommentException );
}

TEST_CASE("Predefined Operatos", "[lexer]") 
{
    Lexer lexer;
    TokenCollection tokens;

    // arithmetic operators
    lexer.tokenize(tokens, "+ - * / % ** ++ --");

    REQUIRE( tokens[0].type() == Token::Type::t_op_add );
    REQUIRE( tokens[1].type() == Token::Type::t_op_sub );
    REQUIRE( tokens[2].type() == Token::Type::t_op_mul );
    REQUIRE( tokens[3].type() == Token::Type::t_op_div );
    REQUIRE( tokens[4].type() == Token::Type::t_op_mod );
    REQUIRE( tokens[5].type() == Token::Type::t_op_pow );
    REQUIRE( tokens[6].type() == Token::Type::t_op_inc );
    REQUIRE( tokens[7].type() == Token::Type::t_op_dec );

    // logical operators
    tokens.clear();
    lexer.tokenize(tokens, "== != < > <= >=");

    REQUIRE( tokens[0].type() == Token::Type::t_logical_eq );
    REQUIRE( tokens[1].type() == Token::Type::t_logical_neq );
    REQUIRE( tokens[2].type() == Token::Type::t_open_angle );
    REQUIRE( tokens[3].type() == Token::Type::t_close_angle );
    REQUIRE( tokens[4].type() == Token::Type::t_logical_leq );
    REQUIRE( tokens[5].type() == Token::Type::t_logical_geq );

    // comperison operators
    tokens.clear();
    lexer.tokenize(tokens, "&& ||");

    REQUIRE( tokens[0].type() == Token::Type::t_logical_and );
    REQUIRE( tokens[1].type() == Token::Type::t_logical_or );
}

TEST_CASE("Predefined Keywords", "[lexer]") 
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(
        tokens, 
        "const " 
        "echo "
        "true "
        "false "
    );

    REQUIRE( tokens[0].type() == Token::Type::t_const );
    REQUIRE( tokens[1].type() == Token::Type::t_echo );
    REQUIRE( tokens[2].type() == Token::Type::t_bool_literal );
    REQUIRE( tokens[3].type() == Token::Type::t_bool_literal );
}

// tests trying to parse custom operator symbols
TEST_CASE( "Operator Prepass", "[lexer]" ) {
    Lexer lexer;
    TokenCollection tokens;
    AST::OperatorRegistry ops;

    std::string code = 
        "operator (int $foo) <=> (int $bar) : int {"
        "    return $foo + $bar;"
        "}"
        "echo 42 <=> 69";

    lexer.tokenize_prepass_operators(code, ops);
    lexer.tokenize(tokens, code, &ops);

    REQUIRE( tokens.tokens.size() == 23 );
    REQUIRE( tokens.tokens[20].type == Token::Type::t_integer_literal );
    REQUIRE( tokens.tokens[21].type == Token::Type::t_op_custom );
    REQUIRE( tokens.tokens[22].type == Token::Type::t_integer_literal );

    REQUIRE( tokens.token_values[21] == "<=>" );
}