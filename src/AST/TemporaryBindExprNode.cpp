#include "AST/TemporaryBindExprNode.h"

#include "AST/VarDeclNode.h"

namespace AST
{

ValueType TemporaryBindExprNode::result_type() const
{
    // the body's, with nothing of its own to add. a type on this node would be a second answer to the
    // body's question, and binding a temporary changes where a value lives, not what it is
    return body != nullptr ? body->result_type() : ValueType::void_type();
}

const std::string TemporaryBindExprNode::node_description()
{
    // everything is in it deliberately: -ar is the only practical way to check a retain/release
    // balance, and this node is where the balance lives now
    std::string desc = "tempbind<" + result_type().get_type_desciption() + ">(";

    for (VarDeclNode *temp : temporaries) {
        desc += temp->node_description() + "; ";
    }

    desc += body->node_description();

    for (auto &drop : teardown) {
        desc += "; " + drop.node()->node_description();
    }

    return desc + ")";
}

};
