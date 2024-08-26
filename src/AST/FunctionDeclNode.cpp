#include "AST/FunctionDeclNode.h"

#include "AST/ASTNamespace.h"

const std::string AST::FunctionDeclNode::node_description()
{
    std::string buffer = "function " + namespaced_func_name() + " -> " + get_return_type_description() + "\n";

    if (args.size() > 0) {
        for (auto arg : args) {
            buffer += " - " + arg->node_description() + "\n";
        }
    }

    if (body) {
        buffer += body->node_description();
    }

    return buffer;
}

const std::string AST::FunctionDeclNode::decorated_func_name() const
{
    std::string decorated_name = "_";

    // root first, so a nested namespace reads in declaration order and the root contributes
    // no empty segment
    if (ast_namespace) {
        for (const auto &segment : ast_namespace->path_segments()) {
            decorated_name += segment + "_";
        }
    }

    decorated_name += func_name() + "Z";

    for (auto arg : args) {
        decorated_name += "Z" + arg->type_node()->type.get_mangled_name();
    }

    return decorated_name;
}

const std::string AST::FunctionDeclNode::namespaced_func_name() const
{
    if (ast_namespace) {
        std::string ns = ast_namespace->full_name();
        if (!ns.empty()) {
            return ns + ECO_NAMESPACE_SEPARATOR + func_name();
        }
    }

    return func_name();
}