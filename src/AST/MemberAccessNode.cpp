#include "AST/MemberAccessNode.h"

#include "AST/VarRefNode.h"

AST::MemberAccessNode::MemberAccessNode(NodeReference base, TokenReference member_name)
    : _base_node(base), _member_name(member_name)
{
}

AST::ValueType AST::MemberAccessNode::result_type() const
{
    if (_base_node.has_type<AST::VarRefNode>()) {
        auto &var_ref = _base_node.get<AST::VarRefNode>();

        // // cannot extract type from primitive types
        // if (var_ref.decl->type_node()->type.is_primitive()) {
        //     return ValueType::void_type();
        // }
        // else {
        //     auto complex = var_ref.decl->type_node()->type.get_complex_type();

        //     // if the property does not exit we return void
        //     if (!complex->has_property(_member_name.value())) {
        //         return ValueType::void_type();
        //     }

        //     return complex->get_property_type(_member_name.value());
        // }
    }
    else if (_base_node.has_type<AST::MemberAccessNode>()) {
        auto &member_access = _base_node.get<AST::MemberAccessNode>();
        return member_access.result_type();
    }

    return ValueType::void_type();
}
