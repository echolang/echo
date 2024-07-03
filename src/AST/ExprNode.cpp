#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

AST::ValueType AST::BinaryExprNode::result_type() const
{   
    if (lhs == nullptr || rhs == nullptr) {
        return AST::ValueType::make_void();
    }

    // if both left and right have the same type then the result type is the same
    if (lhs->result_type() == rhs->result_type()) {
        return lhs->result_type();
    }

    return AST::ValueType::make_void();
}

AST::ValueType AST::FunctionCallExprNode::result_type() const
{
    if (decl == nullptr) {
        return AST::ValueType::make_void();
    }

    return decl->get_return_type();
}