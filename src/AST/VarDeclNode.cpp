#include "AST/VarDeclNode.h"

#include "AST/ExprNode.h"

const std::string AST::VarDeclNode::node_description()
{
    std::string typestr;
    if (_type_node != nullptr) {
        typestr = _type_node->node_description();
    } else {
        typestr = "unknown";
    }

    std::string desc = "vardecl<" + typestr + ">(" + token_varname.value() + ")";

    if (init_expr != nullptr) {
        desc += " = " + init_expr->node_description();
    }

    return desc;
        
}