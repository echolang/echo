#include <catch2/catch_test_macros.hpp>

#include <AST/ASTOps.h>

#include "helpers.h"

#define TEST_ASSERT_OP_LIT_TYPE(index, lit_type) \
    { \
        auto op = registry.get_operator(tm.tokens[index]); \
        REQUIRE(op->type == lit_type); \
    }

TEST_CASE( "predefined token operators", "[AST Ops]" )
{
    auto registry = AST::OperatorRegistry();

    auto tm = EchoTests::tests_make_tokenized_module(
        "= "  // t_assign
        "|| " // t_logical_or
        "&& " // t_logical_and
        "== " // t_logical_eq
        "!= " // t_logical_neq
        "< "  // t_open_angle
        "> "  // t_close_angle
        ">= " // t_logical_geq
        "<= " // t_logical_leq
        "+ "  // t_op_add
        "- "  // t_op_sub
        "* "  // t_op_mul
        "/ "  // t_op_div
        "% "  // t_op_mod
        "** "  // t_op_pow
        "++ " // t_op_inc
        "-- " // t_op_dec
        "this is not an operator"
    );

    TEST_ASSERT_OP_LIT_TYPE(0, Token::Type::t_assign);
    TEST_ASSERT_OP_LIT_TYPE(1, Token::Type::t_logical_or);
    TEST_ASSERT_OP_LIT_TYPE(2, Token::Type::t_logical_and);
    TEST_ASSERT_OP_LIT_TYPE(3, Token::Type::t_logical_eq);
    TEST_ASSERT_OP_LIT_TYPE(4, Token::Type::t_logical_neq);
    TEST_ASSERT_OP_LIT_TYPE(5, Token::Type::t_open_angle);
    TEST_ASSERT_OP_LIT_TYPE(6, Token::Type::t_close_angle);
    TEST_ASSERT_OP_LIT_TYPE(7, Token::Type::t_logical_geq);
    TEST_ASSERT_OP_LIT_TYPE(8, Token::Type::t_logical_leq);
    TEST_ASSERT_OP_LIT_TYPE(9, Token::Type::t_op_add);
    TEST_ASSERT_OP_LIT_TYPE(10, Token::Type::t_op_sub);
    TEST_ASSERT_OP_LIT_TYPE(11, Token::Type::t_op_mul);
    TEST_ASSERT_OP_LIT_TYPE(12, Token::Type::t_op_div);
    TEST_ASSERT_OP_LIT_TYPE(13, Token::Type::t_op_mod);
    TEST_ASSERT_OP_LIT_TYPE(14, Token::Type::t_op_pow);
    TEST_ASSERT_OP_LIT_TYPE(15, Token::Type::t_op_inc);
    TEST_ASSERT_OP_LIT_TYPE(16, Token::Type::t_op_dec);

    // test non existent operator
    REQUIRE(registry.get_operator(tm.tokens[17]) == nullptr);
}


namespace
{
    // match_at needs the bound of the region being parsed, which in a real parse comes off the
    // cursor. here the whole collection is the region
    AST::OperatorRegistry::Match match_from(
        const AST::OperatorRegistry &registry, const AST::Module &tm, size_t index)
    {
        return registry.match_at(tm.tokens[index], tm.tokens.size() - index);
    }
}

TEST_CASE( "a declared symbol is matched as a token sequence", "[AST Ops]" )
{
    auto registry = AST::OperatorRegistry();

    SECTION( "a multi token symbol matches across its tokens" ) {
        // `<=>` is not one token: the lexer sees `<=` and `>`, and nothing in it knows about custom
        // operators. putting the two back together is the registry's job
        auto tm = EchoTests::tests_make_tokenized_module("42 <=> 69");

        // before the declaration the leading `<=` is just the predefined operator it lexes as
        REQUIRE( match_from(registry, tm, 1).token_count == 1 );
        REQUIRE( match_from(registry, tm, 1).op->type == Token::Type::t_logical_leq );

        registry.find_or_declare({"<=", ">"});

        auto match = match_from(registry, tm, 1);
        REQUIRE( match.has() );
        REQUIRE( match.token_count == 2 );
        REQUIRE( match.op->spelling == "<=>" );
        REQUIRE( match.op->is_custom() );
    }

    SECTION( "a word symbol matches whole tokens, never a prefix of one" ) {
        // the trap the old prefix-matching lexer entry had: `mm` matched the front of `mmap`
        auto tm = EchoTests::tests_make_tokenized_module("mm mmap");

        registry.find_or_declare({"mm"});

        REQUIRE( match_from(registry, tm, 0).has() );
        REQUIRE( match_from(registry, tm, 0).token_count == 1 );
        REQUIRE_FALSE( match_from(registry, tm, 1).has() );
    }

    SECTION( "the tokens of one symbol have to be adjacent" ) {
        auto tm = EchoTests::tests_make_tokenized_module("!! ! !");

        registry.find_or_declare({"!", "!"});

        REQUIRE( match_from(registry, tm, 0).has() );
        REQUIRE( match_from(registry, tm, 0).token_count == 2 );

        // `! !` is two tokens with a gap, so it is not the symbol. what it *is* is the built-in
        // prefix `!`, one token - so the assertion is about which symbol was matched rather than
        // about there being none
        auto apart = match_from(registry, tm, 2);
        REQUIRE( apart.has() );
        REQUIRE( apart.token_count == 1 );
        REQUIRE_FALSE( apart.op->is_custom() );
    }

    SECTION( "the longest declared symbol wins" ) {
        auto tm = EchoTests::tests_make_tokenized_module("1cm");

        registry.find_or_declare({"c"});
        registry.find_or_declare({"cm"});

        // `cm` lexes as one identifier, so this is really about declaration order not deciding it
        auto match = match_from(registry, tm, 1);
        REQUIRE( match.has() );
        REQUIRE( match.op->spelling == "cm" );
    }

    SECTION( "a match never runs past the end of the region" ) {
        auto tm = EchoTests::tests_make_tokenized_module("42 <=>");

        registry.find_or_declare({"<=", ">"});

        // the last token of the region is `>`, so a two token symbol starting at `<=` fits exactly
        REQUIRE( match_from(registry, tm, 1).token_count == 2 );

        // ...and does not fit when the region stops one token short, which is what a module holding
        // several files back to back looks like from inside one of them. it falls back to the
        // predefined `<=` rather than reading a token that belongs to the next file
        auto clipped = registry.match_at(tm.tokens[1], 1);
        REQUIRE( clipped.token_count == 1 );
        REQUIRE( clipped.op->type == Token::Type::t_logical_leq );
    }

    SECTION( "declaring a predefined spelling answers with the predefined operator" ) {
        // `operator (Point $a) + (Point $b)` overloads `+`; it must not mint a shadowing custom
        // `+`, which would retype every `$i++` in the program
        AST::Operator *op = registry.find_or_declare({"+"});

        REQUIRE( op != nullptr );
        REQUIRE_FALSE( op->is_custom() );
        REQUIRE( op->type == Token::Type::t_op_add );
        REQUIRE( registry.get_operator("+") == op );
    }

    SECTION( "one symbol is one operator, however often it is declared" ) {
        AST::Operator *first = registry.find_or_declare({"avg"});
        AST::Operator *second = registry.find_or_declare({"avg"});

        REQUIRE( first == second );
        REQUIRE( registry.get_operator("avg") == first );
    }
}

TEST_CASE( "a custom operator carries its own precedence and fixities", "[AST Ops]" )
{
    auto registry = AST::OperatorRegistry();

    AST::Operator *op = registry.find_or_declare({"avg"});
    REQUIRE( op != nullptr );

    // undeclared precedence sits between the bitwise tier and comparison, so `1 + 2 avg 3 + 4`
    // groups its operands before combining them
    REQUIRE( op->precedence.sequence == AST::CUSTOM_OP_DEFAULT_PRECEDENCE );
    REQUIRE( op->precedence.sequence > AST::Operator::get_precedence_for_token(Token::Type::t_op_add).sequence );
    REQUIRE( op->precedence.sequence < AST::Operator::get_precedence_for_token(Token::Type::t_logical_eq).sequence );

    // no declaration has claimed it yet, so the expression parser must not treat it as an operator
    REQUIRE_FALSE( op->is_declared() );

    op->declare_fixity(AST::OpFixity::t_infix);

    REQUIRE( op->is_declared() );
    REQUIRE( op->has_fixity(AST::OpFixity::t_infix) );
    REQUIRE_FALSE( op->has_fixity(AST::OpFixity::t_prefix) );
    REQUIRE_FALSE( op->has_fixity(AST::OpFixity::t_suffix) );
}

TEST_CASE( "the built in precedence tiers keep their order", "[AST Ops]" )
{
    // smaller binds tighter, and the tiers are spaced so a declared number has room between them.
    // the numbers are published in book/concept/operators.md, so this is a contract
    const auto seq = [](Token::Type type) {
        return AST::Operator::get_precedence_for_token(type).sequence;
    };

    REQUIRE( seq(Token::Type::t_op_pow) < seq(Token::Type::t_op_mul) );
    REQUIRE( seq(Token::Type::t_op_mul) < seq(Token::Type::t_op_add) );
    REQUIRE( seq(Token::Type::t_op_add) < seq(Token::Type::t_op_shl) );
    REQUIRE( seq(Token::Type::t_op_shl) < seq(Token::Type::t_logical_eq) );
    REQUIRE( seq(Token::Type::t_logical_eq) < seq(Token::Type::t_logical_and) );
    REQUIRE( seq(Token::Type::t_logical_and) < seq(Token::Type::t_logical_or) );
    REQUIRE( seq(Token::Type::t_logical_or) < seq(Token::Type::t_assign) );

    // zero is what is_expr_token reads as "this token cannot appear in an expression"
    REQUIRE( seq(Token::Type::t_identifier) == 0 );
    REQUIRE( seq(Token::Type::t_op_add) != 0 );
}
