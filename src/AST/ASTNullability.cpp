#include "AST/ASTNullability.h"

#include "AST/ASTModule.h"
#include "AST/ExprNode.h"
#include "AST/NullNode.h"
#include "AST/TypeCastNode.h"

namespace
{
    // looks through the implicit casts the parser and monomorphizer wrap around an argument, to the
    // expression the user actually wrote. an explicit cast stops the walk: the user wrote that one, and
    // `(int32?)null` is a null they meant to give a type to
    AST::ExprNode *strip_implicit_casts(AST::ExprNode *expr)
    {
        while (expr != nullptr && expr->get_node_type() == AST::NodeType::n_type_cast) {
            auto *cast = static_cast<AST::TypeCastNode *>(expr);

            if (!cast->is_implcit) {
                break;
            }

            expr = cast->expr;
        }

        return expr;
    }
}

AST::NullNode *AST::written_null_of(AST::ExprNode *expr)
{
    ExprNode *written = strip_implicit_casts(expr);

    if (written == nullptr || written->get_node_type() != NodeType::n_null) {
        return nullptr;
    }

    return static_cast<NullNode *>(written);
}

bool AST::is_written_null(const AST::ExprNode *expr)
{
    return written_null_of(const_cast<ExprNode *>(expr)) != nullptr;
}

bool AST::destination_admits_null(const AST::ValueType &type)
{
    return type.is_nullable() || type.is_weak();
}

bool AST::bind_null_to(AST::ExprNode *expr, const AST::ValueType &destination)
{
    if (!destination_admits_null(destination)) {
        return false;
    }

    NullNode *null_node = written_null_of(expr);

    if (null_node == nullptr || null_node->is_bound()) {
        return false;
    }

    null_node->bound_type = destination;
    return true;
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

AST::ValueType AST::unwrapped_type_of(const AST::ValueType &type)
{
    if (type.is_weak()) {
        return ValueType::make_non_nullable(type.weak_target());
    }

    return ValueType::make_non_nullable(type);
}
