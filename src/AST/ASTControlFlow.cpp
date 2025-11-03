#include "AST/ASTControlFlow.h"

#include "AST/ASTBuiltin.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"

namespace
{
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

        // a statement that never comes back leaves just as surely as a `return`. which builtins those are
        // is AST::builtin_never_returns's question, not this one's - recognised through AST::BuiltinKind
        // rather than by name, so it cannot drift from what actually stops the program
        if (statement.has_type<AST::FunctionCallExprNode>()) {
            auto *call = statement.get_ptr<AST::FunctionCallExprNode>();

            if (call->decl != nullptr && call->decl->is_builtin()
                && AST::builtin_never_returns(AST::builtin_kind_for(call->decl->builtin.value()))) {
                return AST::ExitKind::t_function;
            }
        }

        // a `break` or a `continue` leaves the scope it is written in - and every scope between it and the
        // loop body - but not the enclosing function. that distinction is the entire reason ExitKind is an
        // ordering. (a labelled `break 2` would still be t_scope: it leaves more scopes, not the function.)
        if (statement.has_type<AST::LoopControlNode>()) {
            return AST::ExitKind::t_scope;
        }

        // **both arms, and both of them leaving.** an `if` with no else falls through by construction, and
        // one arm returning says nothing at all about the other - so this is the only branching shape where
        // the statement after the `if` provably cannot be reached, and codegen agrees: gen_if_statement
        // leaves no merge block at all when every arm terminated its own
        //
        // the *weaker* of the two, so one arm returning and the other breaking says the scope is left but
        // the function is not - which is exactly true, and is the reason this is an ordering and not a bool
        if (statement.has_type<AST::IfStatementNode>()) {
            auto *branch = statement.get_ptr<AST::IfStatementNode>();

            if (branch->if_scope == nullptr || branch->else_scope == nullptr) {
                return AST::ExitKind::t_none;
            }

            return std::min(AST::scope_exit_kind(*branch->if_scope),
                            AST::scope_exit_kind(*branch->else_scope));
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
