#include "AST/ASTNullability.h"

#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/NullNode.h"
#include "AST/TypeCastNode.h"

#include <fmt/format.h>

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

std::string AST::certainly_present_refusal(
    AST::OptionalForm form,
    const AST::ValueType &operand_type
)
{
    if (!is_certainly_present(operand_type)) {
        return {};
    }

    const std::string spelled = operand_type.get_type_desciption();

    switch (form) {
        case OptionalForm::t_guard:
            // a guard that cannot fail reads as a claim that the value might be absent. if it never is
            // then either the type is wrong or the guard is, and either way the author wants to know
            return fmt::format(
                "'guard' needs a value that may be absent, and '{}' always is one - write '{}?' if it "
                "may not be, or drop the guard",
                spelled, spelled);

        case OptionalForm::t_null_coalesce:
            // the right side is dead code. reported rather than folded away, for guard's reason
            return fmt::format(
                "'??' needs a value that may be absent on its left, and '{}' always is one - the "
                "right side could never be reached",
                spelled);

        case OptionalForm::t_optional_chain:
            // the `?` is a lie: the short circuit could never fire, and the reader is being told to
            // expect an absence that cannot happen
            return fmt::format(
                "'?->' needs a value that may be absent, and '{}' always is one - write '->'",
                spelled);
    }

    return {};
}

AST::ExprNode *AST::optional_operand_of(
    AST::ExprNode *expr,
    AST::Module &module,
    const TokenReference &at
)
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

bool AST::arrival_wraps_optional(const AST::ValueType &from, const AST::ValueType &to)
{
    return to.is_wrapped_optional() && !from.is_nullable();
}

AST::ValueType AST::echo_printed_type_of(const AST::ValueType &type)
{
    if (type.is_wrapped_optional() && type.optional_payload().is_primitive()) {
        return type.optional_payload();
    }

    return type;
}

AST::ValueType AST::optional_chain_result_type(const AST::ExprNode *continuation, AST::TypeRegistry &registry)
{
    // a chain with nothing after it is a parse the rest of this function cannot answer for, and the
    // undetermined guard below is already the right answer for it
    const ValueType reached = continuation != nullptr
        ? continuation->result_type()
        : ValueType::make_unknown();

    // a call that answers nothing has nothing to be absent. `$a?->save()` is a statement either way, and
    // wrapping void would invent a value for a statement to discard
    //
    // an undetermined type is a *not yet*: the continuation is still a bare `T`, and wrapping it would
    // intern a pair around a type parameter that the next round is about to replace
    //
    // one question rather than three, because is_undetermined_type *is* unknown, void and
    // still-mentions-a-parameter - the single spelling of "no information"
    if (is_undetermined_type(reached)) {
        return reached;
    }

    return registry.get_or_create_optional(reached);
}

std::string AST::null_operand_refusal(const std::string &operator_spelling)
{
    return fmt::format(
        "no overload of operator '{}' accepts a null operand. If the value may be absent, give it a "
        "nullable type ('T?'); if it is always there, there is nothing to compare against null.",
        operator_spelling);
}
