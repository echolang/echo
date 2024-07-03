#include "AST/VarMutNode.h"
#include "AST/ExprNode.h"

const std::string AST::VarMutNode::node_description()
{
    return "mut " + name_full() + " = " + value_expr->node_description() + "\n";
}