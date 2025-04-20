#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/AttributeNode.h"
#include "AST/TypeDeclNode.h"
#include "Debugging.h"

const std::string AST::ScopeNode::node_description()
{
    std::string result = "Scope\n{\n";
    result += DD::tabbify(node_description_inner(), 2);
    result += "}\n";
    return result;
}

const std::string AST::ScopeNode::node_description_inner()
{
    std::string result = "";
    for (auto &child : children) {
        if (!child.has()) continue;
        result += child.node()->node_description() + "\n";
    }

    // trim away the last newline
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }

    return result;
}

void AST::ScopeNode::declare_variable(VarDeclNode &vardecl)
{
    _declared_variables[vardecl.token_varname.value()] = &vardecl;
}

void AST::ScopeNode::add_vardecl(VarDeclNode &vardecl)
{
    children.push_back(AST::make_ref(vardecl));
    declare_variable(vardecl);
}

void AST::ScopeNode::add_funcdecl(AST::FunctionDeclNode &funcdecl)
{
    // the child list only. a function is *found* through Collector::functions, which holds
    // overload sets rather than one declaration per name; this list is what codegen walks to emit
    // the bodies, so it stays
    children.push_back(AST::make_ref(funcdecl));
}

void AST::ScopeNode::add_typedecl(AST::TypeDeclNode &structdecl)
{
    // the child list only, like add_funcdecl. a type is *found* through the namespace symbol table, so
    // there is no name to register here
    children.push_back(AST::make_ref(structdecl));
}

void AST::ScopeNode::add_attribute(AST::AttributeNode &attribute)
{
    children.push_back(AST::make_ref(attribute));
    _attribute_stack.push_back(&attribute);
}

std::vector<AST::AttributeNode *> AST::ScopeNode::collect_attributes()
{
    // create a copy of the attribute stack
    auto result = _attribute_stack;
    _attribute_stack.clear();

    return result;
}

AST::ScopeNode::VariableLookup AST::ScopeNode::lookup_variable(const std::string &varname) const
{
    auto found = _declared_variables.find(varname);
    if (found != _declared_variables.end()) {
        // where it was declared is answered by the frame that holds it, here - the walk back out below
        // only counts boundaries and cannot tell the root from any other scope it passed through
        return VariableLookup { found->second, 0, is_root() };
    }

    if (is_root()) {
        return VariableLookup {};
    }

    VariableLookup outer = parent().lookup_variable(varname);

    // the flag is set on the way *back out*, by the frame being left rather than by the one that found
    // the declaration - which is what makes it true for every level above the boundary and not just the
    // one immediately past it
    if (is_function_boundary && outer.decl != nullptr) {
        outer.boundaries_crossed++;
    }

    return outer;
}

