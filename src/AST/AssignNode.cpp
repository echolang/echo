#include "AST/AssignNode.h"

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "Debugging.h"

const std::string AST::AssignNode::node_description()
{
    std::string desc = target->node_description() + " = " + value_expr->node_description();

    // both halves of the old value's teardown are printed, including the one that only exists as a bool
    // in codegen. --print-resolved-ast is where "does every value get destroyed exactly once" is
    // answered, and a teardown that does not appear there cannot be checked at all
    if (releases_old) {
        desc += "\n" + DD::tabbify("releases old", 4);
    }

    if (target_bind != nullptr) {
        desc += "\n"
            + DD::tabbify("binds target: " + target_bind->node_description(), 4);
    }

    if (teardown_old != nullptr) {
        desc += "\n"
            + DD::tabbify("destroys old:\n" + DD::tabbify(teardown_old->node_description_inner(), 4), 4);
    }

    return desc;
}
