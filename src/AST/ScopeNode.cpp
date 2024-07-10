#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/FunctionDeclNode.h"
#include "Debugging.h"


const std::string AST::ScopeNode::node_description()
{
    std::string result = "Scope\n{\n";
    for (auto &child : children) {
        result += DD::tabbify(child.node()->node_description(), 2) + "\n";
    }
    result += "}\n";
    return result;
}

void AST::ScopeNode::add_vardecl(VarDeclNode &vardecl)
{
    children.push_back(AST::make_ref(vardecl));
    _declared_variables[vardecl.token_varname.value()] = &vardecl;
}

void AST::ScopeNode::add_funcdecl(AST::FunctionDeclNode &funcdecl)
{
    children.push_back(AST::make_ref(funcdecl));

    if (!funcdecl.is_anonymous()) {
        _declared_functions[funcdecl.func_name()] = &funcdecl;
    }
}

bool AST::ScopeNode::is_varname_taken(const std::string &varname) const
{
    // first check if the variable is declared in the local scope
    auto in_local_scope = _declared_variables.find(varname) != _declared_variables.end();
    if (in_local_scope) {
        return true;
    }

    // if this is not the root scope, check the parent tree
    if (!is_root()) {
        return parent().is_varname_taken(varname);
    }
    
    return false;
}

AST::VarDeclNode *AST::ScopeNode::find_vardecl_by_name(const std::string &varname) const
{
    auto found = _declared_variables.find(varname);
    if (found != _declared_variables.end()) {
        return found->second;
    }

    // if this is not the root scope, check the parent tree
    if (!is_root()) {
        return parent().find_vardecl_by_name(varname);
    }
    
    return nullptr;
}

AST::FunctionDeclNode *AST::ScopeNode::find_funcdecl_by_name(const std::string &funcname) const
{
    auto found = _declared_functions.find(funcname);
    if (found != _declared_functions.end()) {
        return found->second;
    }

    // if this is not the root scope, check the parent tree
    if (!is_root()) {
        return parent().find_funcdecl_by_name(funcname);
    }

    return nullptr;
}