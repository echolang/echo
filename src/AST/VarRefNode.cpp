#include "AST/VarRefNode.h"
#include "AST/ASTException.h"

AST::ValueType AST::VarRefNode::result_type() const
{
    if (is_var())
    {
        return _target_node.get<VarNode>().decl().type();
    }
    else if (is_varmember())
    {
        return _target_node.get<VarMemberNode>().property().type;
    }
    
    throw AST::LogicException::UnexpectedNodeReferenceType();
}