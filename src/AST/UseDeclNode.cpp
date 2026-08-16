#include "AST/UseDeclNode.h"
#include "AST/ASTClone.h"

AST::Node *AST::UseDeclNode::clone(CloneContext &cc) const
{
    return cc.shallow(this);
}
