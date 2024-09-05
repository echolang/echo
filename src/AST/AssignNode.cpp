#include "AST/AssignNode.h"

const std::string AST::AssignNode::node_description()
{
    return target->node_description() + " = " + value_expr->node_description();
}
