#include "AST/MemberMutNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ExprNode.h"

using namespace AST;

const std::string MemberMutNode::node_description()
{
    if (member_access && value_expr) {
        return member_access->node_description() + " = " + value_expr->node_description();
    }
    return "membermut [invalid]";
}
