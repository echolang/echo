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

        // `->` reaches through a pointer to any depth: a `ptr<Point>` base addresses a Point,
        // and so does a `ptr<ptr<Point>>` - the member lives on the struct either way
        // (book/concept/pointers_and_refs_v2.md, "Structs and classes")
        auto var_type = target_type_of(var_ref.result_type());

        // cannot extract type from primitive types
        if (var_type.is_primitive()) {
            return ValueType::void_type();
        }
        else if (var_type.is_struct()) {
            auto complex = var_type.get_complex_type();

            // if the property does not exit we return void
            if (!complex->has_property(_member_name.value())) {
                return ValueType::void_type();
            }

            return complex->get_property_type(_member_name.value());
        }
    }
    else if (_base_node.has_type<AST::MemberAccessNode>()) {
        auto &member_access = _base_node.get<AST::MemberAccessNode>();
        auto base_type = target_type_of(member_access.result_type());

        // Check if base type is a struct
        if (base_type.is_struct()) {
            auto complex = base_type.get_complex_type();
            
            // if the property does not exit we return void
            if (!complex->has_property(_member_name.value())) {
                return ValueType::void_type();
            }

            return complex->get_property_type(_member_name.value());
        }
        
        return ValueType::void_type();
    }
    
    return ValueType::void_type();
}
