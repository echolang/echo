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

    if (ast_namespace) {
        const AST::Namespace *ns = ast_namespace;
        do {
            decorated_name += ns->name() + "_";
            ns = ns->parent();
        } while (ns);
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
        return ast_namespace->name() + "::" + func_name();
    }

    return func_name();
}