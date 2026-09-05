#include "AST/GenericValueExprNode.h"

#include "AST/ASTClone.h"
#include "AST/LiteralValueNode.h"

AST::ValueType AST::GenericValueExprNode::result_type() const
{
    if (param == nullptr || !param->is_value_param()) {
        return ValueType::make_unknown();
    }

    return param->value_type;
}

const std::string AST::GenericValueExprNode::node_description()
{
    return "generic-value<" + (param != nullptr ? param->name : std::string("?")) + ">";
}
