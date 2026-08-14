#include "AST/MatchExprNode.h"

#include "AST/ASTControlFlow.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"

using namespace AST;

bool MatchExprNode::arm_yields_address(const Arm &arm) const
{
    return yields_a_place && arm.value != nullptr && !expression_never_returns(*arm.value);
}

const std::string MatchExprNode::node_description()
{
    // the subject's declaration is in it because it is a declaration nobody wrote: -ar is where a
    // reader checks that the scrutinee is evaluated once, and it is evaluated here or nowhere
    std::string desc = "match<" + result_type().get_type_desciption() + ">("
        + (subject != nullptr ? subject->node_description() : "<none>") + ")\n{\n";

    for (Arm &arm : arms) {
        desc += arm.is_else() ? "else" : arm.case_name;

        // the ordinal rather than the name, because what codegen switches on is the discriminant and
        // this dump is where a reader checks that the pattern resolved to the case they meant
        if (arm.case_ordinal.has_value()) {
            desc += "#" + std::to_string(arm.case_ordinal.value());
        }

        // the bindings live in the arm's scope whichever shape the arm is, so the scope is rendered
        // whenever it holds anything - for a value arm that is the bindings alone, and for a block arm
        // it is the whole body
        if (arm.scope != nullptr && !arm.scope->children.empty()) {
            desc += " " + arm.scope->node_description();
        }

        desc += " => ";
        desc += arm.value != nullptr ? arm.value->node_description() : "void";
        desc += "\n";
    }

    return desc + "}";
}
