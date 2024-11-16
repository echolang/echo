#include "AST/FunctionDeclNode.h"

#include "AST/ASTMangler.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"

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

const std::string AST::FunctionDeclNode::signature_description() const
{
    std::string buffer = namespaced_func_name();

    if (is_generic()) {
        buffer += "<";
        for (size_t i = 0; i < type_parameters.size(); i++) {
            buffer += (i > 0 ? ", " : "") + type_parameters[i]->name;
        }
        buffer += ">";
    }

    buffer += "(";
    for (size_t i = 0; i < args.size(); i++) {
        buffer += (i > 0 ? ", " : "") + args[i]->type().get_type_desciption();
    }
    buffer += ")";

    return buffer;
}

const std::string AST::FunctionDeclNode::decorated_func_name() const
{
    // the mangling itself lives in AST::mangle_function_name, which is the single place that
    // knows how a declaration becomes a symbol. this stays as the spelling every caller already
    // uses
    return AST::mangle_function_name(this);
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