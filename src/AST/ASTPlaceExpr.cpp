#include "AST/ASTPlaceExpr.h"

#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MatchExprNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/TemporaryBindExprNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarRefNode.h"

#include <cassert>

namespace AST
{

bool value_is_an_address(const ExprNode &expr)
{
    if (expr.get_node_type() != NodeType::n_expr_match) {
        return false;
    }

    // **asked of the node, because the answer is in the arms rather than in the tag.** an undecided match
    // answers false, which is the honest reading while the fixpoint is still running: the arms have not
    // agreed yet, and this is asked from inside that fixpoint
    return static_cast<const MatchExprNode &>(expr).yields_a_place;
}

namespace
{

// the last node of a place that only re-addresses existing storage: a variable or a static
// property. null when the expression names no lasting storage at all - a call result, a
// field of one (`Box()->n`)
ExprNode *place_anchor(ExprNode *expr)
{
    while (expr != nullptr) {
        switch (expr->get_node_type()) {
            case NodeType::n_varref:
            case NodeType::n_expr_static_property:
                return expr;

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
            {
                auto *index = static_cast<IndexExprNode *>(expr);

                // after the rewrite the container lives as the operator [] receiver, not
                // `base` (that edge is cleared so PointerAdjuster cannot rewrite it twice).
                // the spine has to follow it or every `$a[0]` looks like a call result:
                // place_outlives_statement goes false and a T& of a local element is
                // TemporaryMember; place_root_of goes null and `return $a[0]` of a by-value
                // array skips the dangling-return gate
                if (index->element_call != nullptr && !index->element_call->arguments.empty()) {
                    expr = index->element_call->arguments[0];
                    break;
                }

                expr = index->base;
                break;
            }

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

}

VarDeclNode *place_root_of(ExprNode *expr)
{
    ExprNode *anchor = place_anchor(expr);

    if (anchor == nullptr || anchor->get_node_type() != NodeType::n_varref) {
        return nullptr;
    }

    auto *var_ref = static_cast<VarRefNode *>(anchor);

    return var_ref->is_var() ? &var_ref->get_var().decl() : nullptr;
}

bool place_outlives_statement(ExprNode *expr)
{
    ExprNode *anchor = place_anchor(expr);

    if (anchor == nullptr) {
        return false;
    }

    if (anchor->get_node_type() == NodeType::n_expr_static_property) {
        return true;
    }

    auto *var_ref = static_cast<VarRefNode *>(anchor);

    return var_ref->is_var();
}

};  // namespace AST
