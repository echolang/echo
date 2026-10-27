#include "AST/StaticPropertyExprNode.h"

#include "AST/VarDeclNode.h"

#include <fmt/core.h>

AST::ValueType AST::static_property_stored_type(
    const AST::ValueType &owner,
    const AST::VarDeclNode *decl,
    AST::TypeRegistry &registry
)
{
    if (decl == nullptr || !decl->has_type()) {
        return AST::ValueType::make_unknown();
    }

    AST::ValueType stored = decl->type();

    if (!AST::contains_type_param(stored) || !owner.has_complex_type()) {
        return stored;
    }

    AST::ComplexType *ct = owner.get_complex_type();

    if (ct->is_instantiated() && ct->template_ref != nullptr
        && ct->template_ref->type_parameters.size() == ct->instantiation_args.size()) {
        return AST::substitute_type(
            stored,
            AST::TypeSubstitution::positional(
                ct->template_ref->type_parameters, ct->instantiation_args),
            registry);
    }

    return stored;
}

AST::ValueType AST::StaticPropertyExprNode::result_type() const
{
    // a failed parse left no declaration. `unknown` rather than `void`, because the two are read
    // differently downstream: `void` is a value that is not there and `unknown` is one nothing has
    // named yet, and a hole in the tree is the second
    if (decl == nullptr || !decl->has_type()) {
        return AST::ValueType::make_unknown();
    }

    // substituted through the owner at parse (and again on clone). the declaration lives on
    // the template, so `Pool<int64>::$last` would otherwise still say `T`
    if (!resolved.is_unknown()) {
        return resolved;
    }

    return decl->type();
}

const std::string AST::StaticPropertyExprNode::node_description()
{
    return fmt::format(
        "static_property<{}>({}::{})",
        result_type().get_type_desciption(),
        owner.get_type_desciption(),
        token_name.value()
    );
}
