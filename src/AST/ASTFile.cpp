#include "AST/ASTFile.h"

std::string AST::File::debug_description() const
{
    return root->node_description();
}