#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarRefNode.h"
#include <map>

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
        // Check if this is a generic function and try to infer concrete types
        if (decl->is_generic() && !arguments.empty()) {
            desc += get_instantiated_function_name();
        } else {
            desc += decl->namespaced_func_name();
        }
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
    
    // For generic functions, try to show the resolved return type
    if (decl && decl->is_generic() && !arguments.empty()) {
        desc += get_inferred_return_type().get_type_desciption();
    } else {
        desc += result_type().get_type_desciption();
    }

    return desc;
}

std::string AST::FunctionCallExprNode::get_instantiated_function_name() const
{
    if (!decl || !decl->is_generic()) {
        return decl ? decl->namespaced_func_name() : token_function_name.value();
    }
    
    auto type_params = infer_type_parameters();
    std::string name = decl->namespaced_func_name();
    
    if (!type_params.empty()) {
        name += "<";
        bool first = true;
        for (const auto& param_name : decl->type_parameters) {
            if (!first) name += ", ";
            first = false;
            
            auto it = type_params.find(param_name);
            if (it != type_params.end()) {
                name += it->second.get_type_desciption();
            } else {
                name += param_name; // fallback to parameter name
            }
        }
        name += ">";
    }
    
    return name;
}

AST::ValueType AST::FunctionCallExprNode::get_inferred_return_type() const
{
    if (!decl || !decl->is_generic()) {
        return result_type();
    }
    
    auto type_params = infer_type_parameters();
    auto return_type = decl->get_return_type();
    
    // If the return type is a type parameter, try to resolve it
    if (return_type.is_type_param()) {
        size_t param_index = return_type.get_type_param_index();
        if (param_index < decl->type_parameters.size()) {
            const std::string& param_name = decl->type_parameters[param_index];
            auto it = type_params.find(param_name);
            if (it != type_params.end()) {
                return it->second;
            }
        }
    }
    
    return return_type;
}

std::map<std::string, AST::ValueType> AST::FunctionCallExprNode::infer_type_parameters() const
{
    std::map<std::string, AST::ValueType> type_mappings;
    
    if (!decl || !decl->is_generic() || arguments.size() != decl->args.size()) {
        return type_mappings;
    }
    
    // Try to infer type parameters from function arguments
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto arg_type = arguments[i]->result_type();
        auto param_type = decl->args[i]->type();
        
        // If the parameter type is a type parameter, map it to the argument type
        if (param_type.is_type_param()) {
            size_t param_index = param_type.get_type_param_index();
            if (param_index < decl->type_parameters.size()) {
                const std::string& param_name = decl->type_parameters[param_index];
                type_mappings[param_name] = arg_type;
            }
        }
    }
    
    return type_mappings;
}

AST::ValueType AST::VarPtrExprNode::result_type() const {
    // Return a pointer version of the VarRefNode's type
    return ValueType::make_pointer(var_ref->result_type());
}

const std::string AST::VarPtrExprNode::node_description() {
    return "ptr<" + result_type().get_type_desciption() + ">(" + var_ref->node_description() + ")";
}