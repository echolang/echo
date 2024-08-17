#include "AST/VarMemberNode.h"

#include "AST/ASTException.h"

const AST::StructDeclNode *AST::VarMemberNode::struct_decl() const
{
    // auto complex = _ref->result_type().get_complex_type().
}

const AST::ComplexType::Property &AST::VarMemberNode::property() const   
{
    // auto result = _ref->result_type();
    // auto complex = result.get_complex_type();

    // if (!complex) {
    //     throw AST::LogicException::UnresolableComplexType();
    // }

    // if (!complex->has_property(_member_index)) {
    //     throw AST::LogicException::InvalidMemberIndex();
    // }

    // return complex->get_property(_member_index);
}