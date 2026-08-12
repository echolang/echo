#include <catch2/catch_test_macros.hpp>

#include <Lexer.h>

#include <vector>

namespace
{
    std::vector<Token::Type> types_of(const TokenCollection &tokens)
    {
        std::vector<Token::Type> out;

        for (const auto &token : tokens.tokens) {
            out.push_back(token.type);
        }

        return out;
    }
}

// **the property the whole change rests on.** interpolation would be a breaking language change if a
// literal that holds no hole grew a second token - every diagnostic golden, every attribute value and
// every `die` message is a single `t_string_literal` today, and 600 corpus cases say so
TEST_CASE("A string with no hole is one token, whichever quote it used", "[lexer]")
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "\"plain\" 'plain' \"a { brace\" \"a } brace\" \"ends with $\"");

    REQUIRE( tokens.tokens.size() == 5 );

    for (const auto &token : tokens.tokens) {
        REQUIRE( token.type == Token::Type::t_string_literal );
    }

    // verbatim, quotes included: decoding is AST::decode_string_literal's, so a code excerpt can
    // still show what was written
    REQUIRE( tokens.token_values[0] == "\"plain\"" );
    REQUIRE( tokens.token_values[2] == "\"a { brace\"" );
}

// a `'` string never interpolates, whatever is in it. that is the escape hatch, and the only place in
// the language where the two quote characters mean different things
TEST_CASE("A single quoted string never interpolates", "[lexer]")
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "'{$name} stays'");

    REQUIRE( tokens.tokens.size() == 1 );
    REQUIRE( tokens.tokens[0].type == Token::Type::t_string_literal );
    REQUIRE( tokens.token_values[0] == "'{$name} stays'" );
}

TEST_CASE("An interpolated literal lexes to a run of tokens", "[lexer]")
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "\"a{$x}b{$y}c\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_string_interp_middle,
        Token::Type::t_varname,
        Token::Type::t_string_interp_end,
    } );

    // the chunks are the raw text *between* the holes, unquoted - there is nothing to strip
    REQUIRE( tokens.token_values[0] == "a" );
    REQUIRE( tokens.token_values[1] == "$x" );
    REQUIRE( tokens.token_values[2] == "b" );
    REQUIRE( tokens.token_values[4] == "c" );

    // and an empty chunk is a real chunk, which is what keeps chunks == holes + 1 true by
    // construction rather than by a check afterwards
    tokens.clear();
    lexer.tokenize(tokens, "\"{$x}{$y}\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_string_interp_middle,
        Token::Type::t_varname,
        Token::Type::t_string_interp_end,
    } );

    REQUIRE( tokens.token_values[0].empty() );
    REQUIRE( tokens.token_values[2].empty() );
    REQUIRE( tokens.token_values[4].empty() );
}

// a hole is lexed by the same engine the top level uses, which is the whole reason LexerEngine exists
// as an object - so a hole holds ordinary Echo and its tokens carry real source positions
TEST_CASE("A hole holds ordinary Echo", "[lexer]")
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "\"v={$p->x + 1}\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_accessorlr,
        Token::Type::t_identifier,
        Token::Type::t_op_add,
        Token::Type::t_integer_literal,
        Token::Type::t_string_interp_end,
    } );

    // a nested string inside a hole is consumed as a string, so the quote in it does not close the
    // literal around it
    tokens.clear();
    lexer.tokenize(tokens, "\"{$m->get(\"k\")}!\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_accessorlr,
        Token::Type::t_identifier,
        Token::Type::t_open_paren,
        Token::Type::t_string_literal,
        Token::Type::t_close_paren,
        Token::Type::t_string_interp_end,
    } );

    REQUIRE( tokens.token_values[7] == "!" );
}

// **the spec split is decided on the token stream, not on the characters**, which is what separates
// `{$x:>8}` from `{$std::io::stdout->fd}` and `{$p:$}` - `::` and `:$` are their own tokens and
// neither introduces one
TEST_CASE("A format spec is the text after a top level colon", "[lexer]")
{
    Lexer lexer;
    TokenCollection tokens;

    lexer.tokenize(tokens, "\"{$x:.2f}\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_string_interp_spec,
        Token::Type::t_string_interp_end,
    } );

    // raw, and deliberately never lexed: `.2f` is not Echo and must not be read as any
    REQUIRE( tokens.token_values[2] == ".2f" );

    tokens.clear();
    lexer.tokenize(tokens, "\"{$std::io::stdout->fd}\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_namespace_sep,
        Token::Type::t_identifier,
        Token::Type::t_namespace_sep,
        Token::Type::t_identifier,
        Token::Type::t_accessorlr,
        Token::Type::t_identifier,
        Token::Type::t_string_interp_end,
    } );

    tokens.clear();
    lexer.tokenize(tokens, "\"{$p:$}\"");

    REQUIRE( types_of(tokens) == std::vector<Token::Type>{
        Token::Type::t_string_interp_begin,
        Token::Type::t_varname,
        Token::Type::t_ptr_of,
        Token::Type::t_string_interp_end,
    } );
}

TEST_CASE("An unterminated interpolation is a located error", "[lexer]")
{
    Lexer lexer;
    TokenCollection tokens;

    REQUIRE_THROWS_AS( lexer.tokenize(tokens, "\"a{$x\""), Lexer::TokenException );

    tokens.clear();
    REQUIRE_THROWS_AS( lexer.tokenize(tokens, "\"a{$x}"), Lexer::TokenException );
}
