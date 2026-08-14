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

    // **how far does control go at this one statement?** the per-statement half of the question; the scope
    // walk below is what turns it into an answer about the scope.
    //
    // has_type rather than type(), in every arm. a NodeReference carries its tag beside its pointer and the
    // two can disagree - a parser error arm hands back a null node with the tag already set, so an
    // `n_if_statement` reached here with nothing behind it. has_type asks both, which is why the return arm
    // was always written with it and the call arm, which dereferences, was one bad parse from a crash
    AST::ExitKind statement_exit_kind(const AST::NodeReference &statement)
    {
        if (statement.has_type<AST::ReturnNode>()) {
            return AST::ExitKind::t_function;
        }

        // a statement that never comes back leaves just as surely as a `return`. which calls those are is
        // AST::expression_never_returns's question, not this one's, and it is asked here rather than
        // answered here so that a `die` written as a `match` arm's value gets the same answer this gets
        if (statement.has_type<AST::FunctionCallExprNode>()) {
            auto *call = statement.get_ptr<AST::FunctionCallExprNode>();

            if (call != nullptr && AST::expression_never_returns(*call)) {
                return AST::ExitKind::t_function;
            }
        }

        // a `break` or a `continue` leaves the scope it is written in - and every scope between it and the
        // loop body - but not the enclosing function. that distinction is the entire reason ExitKind is an
        // ordering. (a labelled `break 2` would still be t_scope: it leaves more scopes, not the function.)
        if (statement.has_type<AST::LoopControlNode>()) {
            return AST::ExitKind::t_scope;
        }

        // the two-armed rule, and branch_exit_kind above is the whole of it
        if (statement.has_type<AST::IfStatementNode>()) {
            auto *branch = statement.get_ptr<AST::IfStatementNode>();

            return branch_exit_kind(branch->if_scope, branch->else_scope);
        }

        // **and the same answer for a `const if` - the one question a transient node owes an arm for.**
        // every other pass that could meet one runs *after* the monomorphizer's fixpoint and therefore
        // never does, but this is asked by Parser::parse_guard and AST::close_constructor_body while the
        // parse is still building the tree, so "AST::ConstFolding has removed it by then" is not available
        // here.
        //
        // it is also *conservative in the right direction*: this reads both arms where the lowering will
        // keep only one, so a `const if` whose taken arm returns and whose untaken arm falls through
        // answers t_none here and t_function later. answering above t_none when it is not certain is the
        // failure mode this file's header refuses, and under-answering only costs a dead trailing statement
        if (statement.has_type<AST::ConstIfNode>()) {
            auto *branch = statement.get_ptr<AST::ConstIfNode>();

            return branch_exit_kind(branch->if_scope, branch->else_scope);
        }

        // **a `match` used as a statement leaves iff every one of its arms does** - the two-armed rule
        // above read N arms wide, and sound for its reason: a match is exhaustive, so the arms are the
        // only ways out and there is no fallthrough path to account for.
        //
        // exhaustiveness is what has to be *known* rather than assumed, and `patterns_decided` is how:
        // AST::MatchResolution refuses a match that does not cover every case, so a decided one covers
        // them. an undecided one - which is what Parser::parse_guard and AST::close_constructor_body see,
        // both asked while the parse is still building the tree - is admitted only when an `else` arm is
        // written, which is a fact about the arms alone. under-answering there costs a dead trailing
        // statement, where over-answering is the failure mode this file's header refuses
        //
        // an arm that produces a *value* hands one back to the match, so control rejoins and the arm does
        // not leave - **unless the value is one of the calls that never comes back**, which is the one
        // shape where the two spellings mean the same thing. `E::none => die('...')` leaves exactly as
        // `E::none => { die('...'); }` does, and answering otherwise here would call a match whose every
        // arm stops the program a statement control falls out of
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

        // a bare nested block leaves for whatever reason its own children do. without this arm a
        // `{ return 1; }` written as a block answered "falls through", and the ownership pass appended a
        // dead second copy of the enclosing scope's drop set behind it
        if (statement.has_type<AST::ScopeNode>()) {
            return AST::scope_exit_kind(*statement.get_ptr<AST::ScopeNode>());
        }

        // **no arm for `n_while_statement`, ever.** a loop may run zero times, so it never guarantees that
        // anything inside it ran - not even `while (true)`, whose condition this does not evaluate. an arm
        // here reading "a loop with no break never falls out" would silently move the constructor parser's
        // answer, which is the caller that must be most certain
        return AST::ExitKind::t_none;
    }
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
