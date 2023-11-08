#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNodeReference.h>
#include <AST/NullNode.h>
#include <AST/ScopeNode.h>

TEST_CASE( "Node References", "[AST]" ) 
{
    auto null_node = AST::NullNode();
    auto scope_node = AST::ScopeNode();

    AST::NodeReference ref1 = AST::make_ref<AST::NullNode>(&null_node);
    AST::NodeReference ref2 = AST::make_ref<AST::ScopeNode>(&scope_node);
    AST::NodeReference ref3 = AST::NodeReference();
    
    REQUIRE( ref1.has() );
    REQUIRE( ref2.has() );
    REQUIRE( !ref3.has() );

    REQUIRE( ref1.has_type<AST::NullNode>() );
    REQUIRE( !ref1.has_type<AST::ScopeNode>() );

    REQUIRE( ref2.has_type<AST::ScopeNode>() );
    REQUIRE( !ref2.has_type<AST::NullNode>() );

    REQUIRE( !ref3.has_type<AST::NullNode>() );
    REQUIRE( !ref3.has_type<AST::ScopeNode>() );

    // resolve references
    auto &ref1_node = ref1.get<AST::NullNode>();
    REQUIRE( ref1_node.node_description() == "NULL" );
}   

TEST_CASE( "Node Reference List", "[AST]" ) 
{
    auto nodes = AST::NodeCollection();
    auto &null_node = nodes.emplace_back<AST::NullNode>();
    auto &scope_node = nodes.emplace_back<AST::ScopeNode>();

    AST::NodeReferenceList reflist;
    reflist.push_back(AST::make_ref<AST::NullNode>(&null_node));
    reflist.push_back(AST::make_ref<AST::ScopeNode>(&scope_node));
    reflist.push_back(AST::NodeReference());

    REQUIRE( reflist[0].has() );
    REQUIRE( reflist[1].has() );
    REQUIRE( !reflist[2].has() );

    REQUIRE( reflist[0].has_type<AST::NullNode>() );
    REQUIRE( !reflist[0].has_type<AST::ScopeNode>() );
}