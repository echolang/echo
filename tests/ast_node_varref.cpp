#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/LiteralValueNode.h>
#include <Parser/ExprParser.h>

#include "helpers.h"


TEST_CASE( "int value extraction", "[AST VarRef]" )
{
    auto tm = EchoTests::tests_make_tokenized_module(
        "$foo"
    );

    // test a var ref
    // auto &lit0 = tm.nodes.emplace_back<AST::VarRefNode

}
