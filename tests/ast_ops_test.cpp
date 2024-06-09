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

    auto tm = EchoTests::tests_make_module_with_content(
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
        "^ "  // t_op_pow
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


TEST_CASE( "custom token operators", "[AST Ops]" ) 
{
    auto registry = AST::OperatorRegistry();
 
    auto tm = EchoTests::tests_make_module_with_content(
        "<=>" 
    );

    registry.register_custom_op("<=>", 10, AST::OpAssociativity::left);

    
}