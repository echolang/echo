#include "AST/TypeAliasDeclNode.h"

#include "AST/ASTClone.h"
#include "AST/ASTTypeParam.h"
#include "AST/TypeNode.h"

const std::string AST::TypeAliasDeclNode::node_description()
{
    std::string params;

    for (const TypeParamDecl *param : type_parameters) {
        params += params.empty() ? "<" : ", ";
        params += param->name;
    }

    if (!params.empty()) {
        params += ">";
    }

    return "typealias(" + name() + params + ") = "
        + (target != nullptr ? target->node_description() : "unresolved");
}

AST::Node *AST::TypeAliasDeclNode::clone(CloneContext &cc) const
{
    TypeAliasDeclNode *c = cc.shallow(this);
    c->target = cc.child(c->target);
    return c;
}
