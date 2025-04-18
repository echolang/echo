#include "AST/FunctionDeclNode.h"

#include "AST/ASTMangler.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"

bool AST::FunctionDeclNode::is_interface_requirement() const
{
    return owner_type != nullptr && owner_type->is_interface_kind();
}

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
    // a method is rendered the way it is written, which means neither the implicit receiver nor the
    // owner's type parameters appear: `Box<T>::map<U>(int32)`, not `map<T, U>(Box<T>&, int32)`. this
    // string reaches NoMatchingOverload, AmbiguousCall, DuplicateFunctionSignature and the debug
    // dumps, so a leaked `$this` would be visible in every one of them
    std::string buffer;

    if (owner_type != nullptr) {
        buffer += owner_type->namespaced_name() + ECO_NAMESPACE_SEPARATOR;
        buffer += func_name();
    }
    else {
        buffer += namespaced_func_name();
    }

    if (own_type_param_count() > 0) {
        buffer += "<";
        for (size_t i = inherited_type_param_count; i < type_parameters.size(); i++) {
            buffer += (i > inherited_type_param_count ? ", " : "") + type_parameters[i]->name;
        }
        buffer += ">";
    }

    buffer += "(";
    for (size_t i = implicit_arg_count(); i < args.size(); i++) {
        buffer += (i > implicit_arg_count() ? ", " : "");

        // `mv` is part of how a call has to be written, so it belongs in the signature a diagnostic
        // shows even though it is not part of the type and not something a call resolves on
        if (args[i]->takes_ownership) {
            buffer += "mv ";
        }

        buffer += args[i]->type().get_type_desciption();
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