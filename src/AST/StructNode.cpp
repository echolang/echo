#include "AST/StructNode.h"

#include "AST/VarDeclNode.h"

#include "Debugging.h"

const std::string AST::StructDeclNode::struct_name() const
{
    if (name_token.has_value()) {
        return name_token.value().value();
    }

    return "[anonymous]";
}

const std::string AST::StructDeclNode::namespaced_struct_name() const
{
    if (ast_namespace) {
        std::string ns = ast_namespace->full_name();
        if (!ns.empty()) {
            return ns + ECO_NAMESPACE_SEPARATOR + struct_name();
        }
    }

    return struct_name();
}

const std::string AST::StructDeclNode::node_description()
{
    std::string result = "struct " + namespaced_struct_name() + "\n{\n";
    result += "properties:\n";
    for (auto prop : _properties) {
        result += DD::tabbify(prop->node_description(), 2) + "\n";
    }
    result += "}\n";
    return result;
}

void AST::StructDeclNode::add_property(VarDeclNode *property)
{
    _properties.push_back(property);
    _complex_type.add_property(property->name(), property->type_node()->type);
}