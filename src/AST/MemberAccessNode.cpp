#include "AST/MemberAccessNode.h"

AST::MemberAccessNode::MemberAccessNode(NodeReference base, TokenReference member_name)
    : _base_node(base), _member_name(member_name)
{
}

AST::ValueType AST::MemberAccessNode::base_target_type() const
{
    // any expression can be a member base, so this asks the base for its type rather than
    // switching on its node class. the old switch only knew a varref and a nested member access,
    // so `$items:$[0]->x` - an IndexExprNode base - fell through to void and every consumer
    // downstream silently degraded: the read threw, the write went untyped and no diagnostic fired
    if (!_base_node.has() || !_base_node.is_expression_node()) {
        return ValueType::make_unknown();
    }

    // `->` reaches through a pointer to any depth: a `ptr<Point>` base addresses a Point, and so
    // does a `ptr<ptr<Point>>` - the member lives on the struct either way. target_type_of, not
    // value_type_of, is what says "every level" (book/concept/pointers_and_refs_v2.md,
    // "Structs and classes")
    return target_type_of(_base_node.unsafe_ptr<AST::ExprNode>()->result_type());
}

AST::ValueType AST::MemberAccessNode::result_type() const
{
    // either *storage* class: a property lives at the same place in the same layout, and reaching it
    // through a class handle rather than into a stack aggregate is gen_member_lvalue's business.
    // an interface is excluded - it declares requirements and stores nothing, so a `->x` over one has
    // no property table to answer from and the type checker reports it as an unknown member
    auto base_type = base_target_type();
    if (!base_type.has_property_layout()) {
        return ValueType::void_type();
    }

    auto *complex = base_type.get_complex_type();

    // an unknown member has no type of its own; the type checker reports it by name
    if (complex == nullptr || !complex->has_property(_member_name.value())) {
        return ValueType::void_type();
    }

    return complex->get_property_type(_member_name.value());
}
