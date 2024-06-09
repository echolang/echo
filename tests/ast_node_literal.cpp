#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>


#include "helpers.h"

#define TEST_ASSERT_LIT_DESC(index, type, desc) \
    { \
        auto &lit = tm.nodes.emplace_back<type>(tm.tokens[index]); \
        REQUIRE(lit.node_description() == desc); \
    }

TEST_CASE( "node description", "[AST Literal]" ) 
{
    try {
    
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
    catch (const std::exception &e) {
        std::cout << e.what() << std::endl;
    }
}

TEST_CASE( "float value extraction", "[AST Literal]" ) 
{
    auto tm = EchoTests::tests_make_module_with_content(
        "3.14 " // double literal
        "-3.14 " // double literal
        "3.14f " // float literal
        "-3.14f " // float literal
    );

    auto &lit0 = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[0]);

    REQUIRE(lit0.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64);
    REQUIRE(lit0.get_fvalue_string() == "3.14");
    REQUIRE(lit0.double_value() == 3.14);

    auto &lit1 = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[1]);

    REQUIRE(lit1.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64);
    REQUIRE(lit1.get_fvalue_string() == "-3.14");
    REQUIRE(lit1.double_value() == -3.14);

    auto &lit2 = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[2]);

    REQUIRE(lit2.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float32);
    REQUIRE(lit2.get_fvalue_string() == "3.14");
    REQUIRE(lit2.float_value() == 3.14f);

    auto &lit3 = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[3]);

    REQUIRE(lit3.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float32);
    REQUIRE(lit3.get_fvalue_string() == "-3.14");
    REQUIRE(lit3.float_value() == -3.14f);
}


TEST_CASE( "float value with expectation", "[AST Literal]" ) 
{
    auto tm = EchoTests::tests_make_module_with_content(
        "3.14 " // double literal
        "3.14f " // float literal
    );

    auto &lit0d = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[0], AST::ValueTypePrimitive::t_float64);

    REQUIRE(lit0d.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64);
    REQUIRE(lit0d.double_value() == 3.14);

    auto &lit0f = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[0], AST::ValueTypePrimitive::t_float32);

    REQUIRE(lit0f.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float32);
    REQUIRE(lit0f.float_value() == 3.14f);

    auto &lit1d = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[1], AST::ValueTypePrimitive::t_float64);

    REQUIRE(lit1d.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64);
    REQUIRE(lit1d.double_value() == 3.14);

    auto &lit1f = tm.nodes.emplace_back<AST::LiteralFloatExprNode>(tm.tokens[1], AST::ValueTypePrimitive::t_float32);

    REQUIRE(lit1f.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float32);
    REQUIRE(lit1f.float_value() == 3.14f);
}

TEST_CASE( "int value extraction", "[AST Literal]" ) 
{
    auto tm = EchoTests::tests_make_module_with_content(
        "42 " // int literal
        "-42 " // int literal
    );

    auto &lit0 = tm.nodes.emplace_back<AST::LiteralIntExprNode>(tm.tokens[0]);

}