#include "AST/VarRefNode.h"
#include "AST/ASTException.h"

AST::ValueType AST::VarRefNode::result_type() const
{
    if (is_var())
    {
        auto &decl = _target_node.get<VarNode>().decl();

        // a decl whose type inference failed (e.g. its initializer had an error)
        // never gets a type node; report unknown rather than dereferencing null
        if (!decl.has_type()) {
            return ValueType::make_unknown();
        }

        return decl.type();
    }

    throw AST::LogicException::UnexpectedNodeReferenceType();
}