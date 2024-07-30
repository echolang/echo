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

const std::string AST::FunctionCallExprNode::decorated_func_name() const
{
    return decl ? decl->decorated_func_name() : token_function_name.value();
}

const std::string AST::FunctionCallExprNode::node_description()
{
    std::string desc = "call ";

    if (decl) {
        desc += decl->namespaced_func_name();
    } else {
        desc += token_function_name.value();
    }

    desc += "(";

    for (auto arg : arguments) {
        desc += arg->node_description() + ", ";
    }

    if (arguments.size() > 0) {
        desc = desc.substr(0, desc.size() - 2);
    }

    desc += "): ";
    desc += result_type().get_type_desciption();

    return desc;
}