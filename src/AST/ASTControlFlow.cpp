#include "AST/ASTControlFlow.h"

#include "AST/ASTBuiltin.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstExprNode.h"
#include "AST/LoopControlNode.h"
#include "AST/MatchExprNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"

#include <algorithm>

namespace
{
    // **both arms, and both of them leaving.** a two-armed branch with no `else` falls through by
    // construction, and one arm returning says nothing at all about the other - so this is the only
    // branching shape where the statement after it provably cannot be reached, and codegen agrees:
    // gen_if_statement leaves no merge block at all when every arm terminated its own
    //
    // the *weaker* of the two, so one arm returning and the other breaking says the scope is left but the
    // function is not - which is exactly true, and is the reason this is an ordering and not a bool
    AST::ExitKind branch_exit_kind(const AST::ScopeNode *if_scope, const AST::ScopeNode *else_scope)
    {
        if (if_scope == nullptr || else_scope == nullptr) {
            return AST::ExitKind::t_none;
        }

        return std::min(AST::scope_exit_kind(*if_scope), AST::scope_exit_kind(*else_scope));
    }
};

// has_type rather than type(): a parser error can hand back a null node with the tag already set
AST::ExitKind AST::statement_exit_kind(const AST::NodeReference &statement)
{
    if (statement.has_type<AST::ReturnNode>()) {
        return AST::ExitKind::t_function;
    }

    // die() leaves like return; expression_never_returns is the owner
    if (statement.has_type<AST::FunctionCallExprNode>()) {
        auto *call = statement.get_ptr<AST::FunctionCallExprNode>();

        if (call != nullptr && AST::expression_never_returns(*call)) {
            return AST::ExitKind::t_function;
        }
    }

    // break/continue leave the scope, not the function
    if (statement.has_type<AST::LoopControlNode>()) {
        return AST::ExitKind::t_scope;
    }

    if (statement.has_type<AST::IfStatementNode>()) {
        auto *branch = statement.get_ptr<AST::IfStatementNode>();

        return branch_exit_kind(branch->if_scope, branch->else_scope);
    }

    // same two-armed rule; both arms because this is asked at parse, before ConstFolding
    if (statement.has_type<AST::ConstIfNode>()) {
        auto *branch = statement.get_ptr<AST::ConstIfNode>();

        return branch_exit_kind(branch->if_scope, branch->else_scope);
    }

    // leaves iff every arm does. undecided matches only count with an else.
    // a value arm rejoins unless the value never returns
    if (statement.has_type<AST::MatchExprNode>()) {
        auto *node = statement.get_ptr<AST::MatchExprNode>();

        const bool has_else = std::any_of(
            node->arms.begin(), node->arms.end(),
            [](const AST::MatchExprNode::Arm &arm) { return arm.is_else(); });

        if (node->arms.empty() || !(node->patterns_decided || has_else)) {
            return AST::ExitKind::t_none;
        }

        AST::ExitKind kind = AST::ExitKind::t_function;

        for (const AST::MatchExprNode::Arm &arm : node->arms) {
            if (arm.value != nullptr) {
                if (!AST::expression_never_returns(*arm.value)) {
                    return AST::ExitKind::t_none;
                }

                continue;
            }

            if (arm.scope == nullptr) {
                return AST::ExitKind::t_none;
            }

            kind = std::min(kind, AST::scope_exit_kind(*arm.scope));
        }

        return kind;
    }

    if (statement.has_type<AST::ScopeNode>()) {
        return AST::scope_exit_kind(*statement.get_ptr<AST::ScopeNode>());
    }

    // a loop may run zero times, including `while (true)`
    return AST::ExitKind::t_none;
}

bool AST::expression_never_returns(const AST::ExprNode &expr)
{
    if (expr.get_node_type() != NodeType::n_expr_call) {
        return false;
    }

    const auto &call = static_cast<const FunctionCallExprNode &>(expr);

    // a null `decl` is legitimate - an unresolved call is what every round before the last one holds - and
    // the honest answer for one is "not known to stop the program". the fixpoint asks again next round
    if (call.decl == nullptr || !call.decl->is_builtin()) {
        return false;
    }

    return builtin_never_returns(builtin_kind_for(call.decl->builtin.value()));
}

AST::ExitKind AST::scope_exit_kind(const AST::ScopeNode &scope)
{
    // **the first statement that leaves decides, and the walk stops there.** everything written after it is
    // unreachable, so it cannot contribute an answer - `{ break; return $this; }` leaves the *scope*, and a
    // walk that kept looking would find the dead `return` and say the function was done
    for (const NodeReference &child : scope.children) {
        const ExitKind kind = statement_exit_kind(child);

        if (kind != ExitKind::t_none) {
            return kind;
        }
    }

    return ExitKind::t_none;
}
