#include "AST/ASTPlaceExpr.h"

#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MatchExprNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/TemporaryBindExprNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarRefNode.h"

#include <cassert>
#include <unordered_set>

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

ExprNode *member_base(ExprNode *expr)
{
    auto &base = static_cast<MemberAccessNode *>(expr)->get_base_node();

    return base.has() && base.is_expression_node() ? base.unsafe_ptr<ExprNode>() : nullptr;
}

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
                expr = member_base(expr);
                break;

            default:
                return nullptr;
        }
    }

    return nullptr;
}

}

bool is_unaccounted_storage(const ExprNode &expr)
{
    const ExprNode *cur = strip_implicit_casts(&expr);

    while (cur != nullptr && cur->get_node_type() == NodeType::n_member_access) {
        cur = strip_implicit_casts(member_base(const_cast<ExprNode *>(cur)));
    }

    if (cur == nullptr) {
        return false;
    }

    if (cur->get_node_type() == NodeType::n_expr_deref) {
        return true;
    }

    if (cur->get_node_type() == NodeType::n_expr_index) {
        return static_cast<const IndexExprNode *>(cur)->indexed_base_type().is_pointer();
    }

    return false;
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

namespace
{

bool is_pointer_typed_parameter(const VarDeclNode *decl, const FunctionDeclNode *function)
{
    if (decl == nullptr || function == nullptr) {
        return false;
    }

    for (auto *arg : function->args) {
        if (arg == decl && arg->has_type() && arg->type().is_pointer()) {
            return true;
        }
    }

    return false;
}

// T& / const T& local: non-nullable pointer, not a parameter. ptr<T> copies bits
bool is_borrow_local(const VarDeclNode *decl, const FunctionDeclNode *function)
{
    if (decl == nullptr || !decl->has_type()) {
        return false;
    }

    if (is_pointer_typed_parameter(decl, function)) {
        return false;
    }

    const ValueType type = decl->type();
    return type.is_pointer() && !type.is_nullable();
}

VarDeclNode *names_callee_frame_storage_walk(
    ExprNode *expr,
    FunctionDeclNode *function,
    std::unordered_set<const VarDeclNode *> &visited
)
{
    expr = strip_implicit_casts(expr);

    if (expr == nullptr) {
        return nullptr;
    }

    if (expr->get_node_type() == NodeType::n_expr_addrof) {
        ExprNode *operand = strip_implicit_casts(static_cast<AddrOfExprNode *>(expr)->operand);

        if (operand == nullptr) {
            return nullptr;
        }

        // the address arrived through a pointer the compiler does not own
        if (is_unaccounted_storage(*operand)) {
            return nullptr;
        }

        VarDeclNode *root = place_root_of(operand);

        if (root == nullptr) {
            return nullptr;
        }

        if (is_pointer_typed_parameter(root, function)) {
            return nullptr;
        }

        // `&$t` names the T& slot (callee-frame). `$t->field` is the address the local
        // was bound to, so a projection walks the initializer
        if (is_borrow_local(root, function)
            && operand->get_node_type() != NodeType::n_varref) {
            if (!visited.insert(root).second) {
                return root;
            }

            return names_callee_frame_storage_walk(root->init_expr, function, visited);
        }

        return root;
    }

    if (expr->get_node_type() == NodeType::n_varref) {
        VarDeclNode *root = place_root_of(expr);

        if (is_borrow_local(root, function)) {
            if (!visited.insert(root).second) {
                return root;
            }

            return names_callee_frame_storage_walk(root->init_expr, function, visited);
        }
    }

    // a load of pointer bits copies an address; it is not callee-frame storage
    return nullptr;
}

}

VarDeclNode *names_callee_frame_storage(ExprNode *expr, FunctionDeclNode *function)
{
    if (expr == nullptr || function == nullptr) {
        return nullptr;
    }

    std::unordered_set<const VarDeclNode *> visited;
    return names_callee_frame_storage_walk(expr, function, visited);
}

};  // namespace AST
