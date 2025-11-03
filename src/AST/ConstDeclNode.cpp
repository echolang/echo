#include "AST/ConstDeclNode.h"

#include "AST/ConstRefExprNode.h"
#include "AST/ExprNode.h"

const std::string AST::ConstDeclNode::node_description()
{
    std::string typestr = _type_node != nullptr ? _type_node->node_description() : "inferred";

    std::string desc = "constdecl<" + typestr + ">(" + token_name.value() + ")";

    if (value != nullptr) {
        desc += " = " + value->node_description();
    }

    return desc;
}

AST::ValueType AST::ConstRefExprNode::result_type() const
{
    // **unknown until the expander has been**, deliberately: this node names a constant, and what the
    // constant *is* is the expression that gets cloned in its place. Answering the declaration's type here
    // instead would be a second answer to that question, and one that a use site could read before the
    // initializer it belongs to had been expanded itself
    return ValueType::make_unknown();
}

const std::string AST::ConstRefExprNode::node_description()
{
    return "constref(" + token_name.value() + ")";
}
