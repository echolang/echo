#include "AST/ASTControlFlow.h"

#include "AST/ASTBuiltin.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"

namespace
{
    // **does control leave for good at this one statement?** the per-statement half of the question; the
    // scope walk below is what turns it into an answer about the scope.
    //
    // has_type rather than type(), in all three arms. a NodeReference carries its tag beside its pointer
    // and the two can disagree - a parser error arm hands back a null node with the tag already set, so an
    // `n_if_statement` reached here with nothing behind it. has_type asks both, which is why the return
    // arm was always written with it and the call arm, which dereferences, was one bad parse from a crash
    bool statement_always_exits(const AST::NodeReference &statement)
    {
        if (statement.has_type<AST::ReturnNode>()) {
            return true;
        }

        // a `die` never comes back, so a statement that is one leaves just as surely as a `return`.
        // recognised through AST::BuiltinKind rather than by name, so it cannot drift from what actually
        // stops the program - and `assert` is deliberately *not* on the list, it returns when it holds
        if (statement.has_type<AST::FunctionCallExprNode>()) {
            auto *call = statement.get_ptr<AST::FunctionCallExprNode>();

            if (call->decl != nullptr && call->decl->is_builtin()
                && AST::builtin_kind_for(call->decl->builtin.value()) == AST::BuiltinKind::t_die) {
                return true;
            }
        }

        // **both arms, and both of them leaving.** an `if` with no else falls through by construction, and
        // one arm returning says nothing at all about the other - so this is the only branching shape where
        // the statement after the `if` provably cannot be reached, and codegen agrees: gen_if_statement
        // leaves no merge block at all when every arm terminated its own
        //
        // recursive through scope_always_exits, so an `if/else` nested in an arm answers too, terminating
        // on the depth of the tree
        if (statement.has_type<AST::IfStatementNode>()) {
            auto *branch = statement.get_ptr<AST::IfStatementNode>();

            return branch->if_scope != nullptr
                && branch->else_scope != nullptr
                && AST::scope_always_exits(*branch->if_scope)
                && AST::scope_always_exits(*branch->else_scope);
        }

        return false;
    }
}

bool AST::scope_always_exits(const AST::ScopeNode &scope)
{
    // **any statement, not just the last one.** once one of them leaves for good, everything written after
    // it is unreachable - and so is the point past the closing brace, which is the thing both callers are
    // actually asking about
    //
    // that distinction is not academic. asking only the last statement, a `return` followed by so much as
    // one dead declaration answered false, and the ownership pass appended a second copy of the drop set
    // it had already collected onto that return's unwind. the tail is exactly where a stray statement is
    // most likely to be, which is why this is a walk
    //
    // it stays sound with no `break` or `continue` in the language: nothing can re-enter a scope below a
    // statement that left it. **the day one lands, this becomes wrong** and has to learn that a loop body's
    // exit is not the enclosing function's - the same edge AST::OwnershipPass::walk_scope will grow
    for (const NodeReference &child : scope.children) {
        if (statement_always_exits(child)) {
            return true;
        }
    }

    return false;
}
