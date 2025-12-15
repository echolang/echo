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

};  // namespace AST
