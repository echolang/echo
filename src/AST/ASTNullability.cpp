#include "AST/ASTNullability.h"

#include "AST/ASTBuiltin.h"
#include "AST/ASTModule.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"

namespace
{
    // looks through the implicit casts the parser and monomorphizer wrap around an argument, to the
    // expression the user actually wrote. an explicit cast stops the walk: the user wrote that one, and
    // `(int32?)null` is a null they meant to give a type to
    const AST::ExprNode *strip_implicit_casts(const AST::ExprNode *expr)
    {
        while (expr != nullptr && expr->get_node_type() == AST::NodeType::n_type_cast) {
            const auto *cast = static_cast<const AST::TypeCastNode *>(expr);

            if (!cast->is_implcit) {
                break;
            }

            expr = cast->expr;
        }

        return expr;
    }
}

bool AST::is_written_null(const AST::ExprNode *expr)
{
    const ExprNode *written = strip_implicit_casts(expr);
    return written != nullptr && written->get_node_type() == NodeType::n_null;
}

bool AST::is_certainly_present(const AST::ValueType &type)
{
    return !type.is_nullable() && !is_undetermined_type(type);
}

AST::ExprNode *AST::optional_operand_of(
    AST::ExprNode *expr, AST::Module &module, const TokenReference &at)
{
    if (expr == nullptr) {
        return nullptr;
    }

    const ValueType type = expr->result_type();

    // a weak is upgraded first, and then it is an ordinary nullable like any other
    if (type.is_weak()) {
        return &module.nodes.emplace_back<StrongExprNode>(expr, at);
    }

    return expr;
}

bool AST::scope_always_exits(const AST::ScopeNode &scope)
{
    if (scope.children.empty()) {
        return false;
    }

    const NodeReference &last = scope.children.back();

    if (last.type() == NodeType::n_func_return) {
        return true;
    }

    // a `die` never comes back, so a scope ending in one leaves just as surely as one ending in `return`.
    // recognised through AST::BuiltinKind rather than by name, so it cannot drift from what actually
    // stops the program - and `assert` is deliberately *not* on the list, because it returns when it holds
    if (last.type() == NodeType::n_expr_call) {
        auto *call = last.unsafe_ptr<FunctionCallExprNode>();

        if (call->decl != nullptr && call->decl->is_builtin()
            && builtin_kind_for(call->decl->builtin.value()) == BuiltinKind::t_die) {
            return true;
        }
    }

    return false;
}

AST::ValueType AST::unwrapped_type_of(const AST::ValueType &type)
{
    if (type.is_weak()) {
        return ValueType::make_non_nullable(type.weak_target());
    }

    return ValueType::make_non_nullable(type);
}
