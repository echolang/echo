#include "AST/TypeDeclNode.h"

#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTTypeParam.h"

#include "Debugging.h"

const std::string AST::TypeDeclNode::type_name() const
{
    if (name_token.has_value()) {
        return name_token.value().value();
    }

    return "[anonymous]";
}

const std::string AST::TypeDeclNode::namespaced_type_name() const
{
    if (ast_namespace) {
        std::string ns = ast_namespace->full_name();
        if (!ns.empty()) {
            return ns + ECO_NAMESPACE_SEPARATOR + type_name();
        }
    }

    return type_name();
}

const std::string AST::TypeDeclNode::node_description()
{
    // the keyword the declaration was written with, so --print-ast says which of the kinds this
    // is. printing an interface as `struct` would make the one dump that could show the
    // difference say there was none - AST::type_kind_keyword is why a fifth kind cannot repeat it
    //
    // an incomplete type has no body, so it must not print one: the dump would otherwise look
    // like an empty struct, which is a different type with a layout of size zero
    if (complex_type().is_opaque_kind()) {
        return "extern struct " + namespaced_type_name() + ";";
    }

    std::string result =
        std::string(AST::type_kind_keyword(complex_type().kind)) + " " + namespaced_type_name() + "\n{\n";

    // ahead of the members, which is the order they are declared in and the order the parser enforces:
    // a requirement's signature may mention one, so it has to be a name before one is read
    if (!complex_type().associated_types().empty()) {
        result += "associated types:\n";
        for (const auto *assoc : complex_type().associated_types()) {
            result += DD::tabbify(assoc->describe() + " : " + assoc->constraint_spelling, 2) + "\n";
        }
    }

    result += "properties:\n";
    for (auto prop : _properties) {
        result += DD::tabbify(prop->node_description(), 2) + "\n";
    }
    if (!methods().empty()) {
        result += "methods:\n";
        for (auto method : methods()) {
            // the signature only. the body is emitted where the declaration lands, in the
            // enclosing scope's children, so printing it here would print it twice
            result += DD::tabbify(method->signature_description(), 2) + "\n";
        }
    }

    // listed separately because it is not in the method table - it is not a name a call site can
    // spell, and it must not read as one more overload candidate
    if (auto *dtor = complex_type().destructor()) {
        result += "destructor:\n";
        result += DD::tabbify(dtor->signature_description(), 2) + "\n";
    }

    result += "}\n";
    return result;
}

void AST::TypeDeclNode::add_property(VarDeclNode *property)
{
    _properties.push_back(property);
    _complex_type.add_property(property->name(), property->type_node()->type, property->visibility);
}
