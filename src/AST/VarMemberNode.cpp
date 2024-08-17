#include "AST/VarMemberNode.h"

#include "AST/ASTException.h"
#include "AST/VarRefNode.h"

const std::string AST::VarMemberNode::node_description()
{
    std::string member = _token_member.has_value() ? _token_member.value().value() : "?";
    return "varmember(" + _ref->node_description() + "->" + member + ")";
}

const AST::StructDeclNode *AST::VarMemberNode::struct_decl() const
{
    // For now, we'll return nullptr and implement this properly
    // when we have the full context during compilation
    return nullptr;
}

const AST::ComplexType::Property &AST::VarMemberNode::property() const   
{
    auto result = _ref->result_type();
    
    if (!result.is_struct() && !result.is_class()) {
        throw AST::LogicException::UnresolableComplexType();
    }
    
    auto complex = result.get_complex_type();

    if (!complex) {
        throw AST::LogicException::UnresolableComplexType();
    }

    if (!complex->has_property(_member_index)) {
        throw AST::LogicException::InvalidMemberIndex();
    }

    return complex->get_property(_member_index);
}