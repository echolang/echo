#include "AST/LiteralValueNode.h"

std::string AST::LiteralStringExprNode::get_string_value() const
{
    return token_literal.value().substr(1, token_literal.value().size() - 2);
}