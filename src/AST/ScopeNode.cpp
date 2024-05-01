#include "AST/ScopeNode.h"
#include "Debugging.h"

const std::string AST::ScopeNode::node_description()
{
    std::string result = "Scope\n{\n";
    for (auto &child : children) {
        result += DD::tabbify(child.node()->node_description(), 2) + "\n";
    }
    result += "}\n";
    return result;
}