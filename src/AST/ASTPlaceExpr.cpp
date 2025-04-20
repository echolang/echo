#include "AST/ASTPlaceExpr.h"

#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/TemporaryBindExprNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarRefNode.h"

#include <cassert>

namespace AST
{

VarDeclNode *place_root_of(ExprNode *expr)
{
    while (expr != nullptr) {
        switch (expr->get_node_type()) {
            case NodeType::n_varref:
            {
                auto *var_ref = static_cast<VarRefNode *>(expr);
                return var_ref->is_var() ? &var_ref->get_var().decl() : nullptr;
            }

            case NodeType::n_expr_addrof:
                expr = static_cast<AddrOfExprNode *>(expr)->operand;
                break;

            case NodeType::n_expr_deref:
                expr = static_cast<DerefExprNode *>(expr)->operand;
                break;

            case NodeType::n_expr_peel:
                expr = static_cast<PointerValueNode *>(expr)->operand;
                break;

            case NodeType::n_expr_index:
                expr = static_cast<IndexExprNode *>(expr)->base;
                break;

            case NodeType::n_member_access:
            {
                auto &base = static_cast<MemberAccessNode *>(expr)->get_base_node();
                expr = base.has() && base.is_expression_node() ? base.unsafe_ptr<ExprNode>() : nullptr;
                break;
            }

            default:
                return nullptr;
        }
    }

    return nullptr;
}

const TokenReference &location_of_expression(ExprNode *expr)
{
    switch (expr->get_node_type()) {
        case NodeType::n_member_access:
            return static_cast<MemberAccessNode *>(expr)->get_member_name();

        case NodeType::n_expr_index:
            return static_cast<IndexExprNode *>(expr)->token_bracket;

        case NodeType::n_expr_peel:
            return static_cast<PointerValueNode *>(expr)->token_peel;

        case NodeType::n_expr_move:
            return static_cast<MoveExprNode *>(expr)->token_move;

        case NodeType::n_expr_call:
            return static_cast<FunctionCallExprNode *>(expr)->token_function_name;

        // the member name that asked for the temporary, which is where every other diagnostic
        // about one points too. reachable as a *statement* - `$o->mid()->handle;` - so the
        // discarded-temporary arm asks this about it
        case NodeType::n_expr_temp_bind:
            return static_cast<TemporaryBindExprNode *>(expr)->token;

        // `&` carries no token of its own, so it borrows its operand's - which for the receiver
        // this pass gives storage to is the called function's name
        case NodeType::n_expr_addrof:
            return location_of_expression(static_cast<AddrOfExprNode *>(expr)->operand);

        case NodeType::n_expr_deref:
            return location_of_expression(static_cast<DerefExprNode *>(expr)->operand);

        // the four value shapes a temporary can be minted for since todo/A13c. each carries the
        // token the author wrote, which is where a diagnostic about the storage it was given
        // belongs - `inc(41)` points at the `41`
        //
        // the three numeric literals share LiteralPrimitiveExprNode, so one arm answers all of them;
        // a string literal is its own class with the same field name
        case NodeType::n_literal:
        case NodeType::n_literal_float:
        case NodeType::n_literal_int:
        case NodeType::n_literal_bool:
            return static_cast<LiteralPrimitiveExprNode *>(expr)->token_literal;

        case NodeType::n_literal_string:
            return static_cast<LiteralStringExprNode *>(expr)->token_literal;

        // an operator carries the token, not the expression - so `$a + $b` locates at the `+`, which
        // is the one character that names the whole result
        case NodeType::n_expr_binary:
            return static_cast<BinaryExprNode *>(expr)->op_node->token_literal;

        case NodeType::n_expr_unary:
            return static_cast<UnaryExprNode *>(expr)->token_operator;

        // a cast has no token of its own - an implicit one was never written at all - so it borrows
        // its operand's, exactly as `&` does above
        case NodeType::n_type_cast:
            return location_of_expression(static_cast<TypeCastNode *>(expr)->expr);

        case NodeType::n_varref:
            return static_cast<VarRefNode *>(expr)->get_var().use_token();

        // the three nullability forms each carry the token they were written at, which is the right
        // place for a diagnostic about a temporary one of them asked for: the `?->`, the `??`, the
        // `strong`
        case NodeType::n_expr_optional_chain:
            return static_cast<OptionalChainExprNode *>(expr)->token;

        case NodeType::n_expr_null_coalesce:
            return static_cast<NullCoalesceExprNode *>(expr)->token;

        case NodeType::n_expr_strong:
            return static_cast<StrongExprNode *>(expr)->token;

        case NodeType::n_expr_chain_base:
            return static_cast<ChainBaseNode *>(expr)->token;

        default:
            assert(false && "no token to locate this expression at");
            return static_cast<MoveExprNode *>(expr)->token_move;
    }
}

};  // namespace AST
