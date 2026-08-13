#include "AST/StaticPropertyExprNode.h"

#include "AST/VarDeclNode.h"

#include <fmt/core.h>

AST::ValueType AST::StaticPropertyExprNode::result_type() const
{
    // a failed parse left no declaration. `unknown` rather than `void`, because the two are read
    // differently downstream: `void` is a value that is not there and `unknown` is one nothing has
    // named yet, and a hole in the tree is the second
    if (decl == nullptr || !decl->has_type()) {
        return AST::ValueType::make_unknown();
    }

    // the declared type as written. **not substituted through the owner here**, and it does not need
    // to be: the declaration reached through `owner` is already the instantiation's, because
    // AST::OwnershipPass synthesizes a static's storage per instantiated type rather than per template
    // - so a `Box<int32>::$count` was resolved against `Box<int32>`'s own list
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
