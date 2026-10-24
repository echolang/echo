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

// the last node of a place that only re-addresses existing storage: a variable, a static
// property, or a `?->` marker. null when the expression names no lasting storage at all -
// a call result, a field of one (`Box()->n`)
ExprNode *place_anchor(ExprNode *expr)
{
    while (expr != nullptr) {
        switch (expr->get_node_type()) {
            case NodeType::n_varref:
            case NodeType::n_expr_static_property:
            // the unwrapped base of a `?->`: a place, and the leaf `chain_base_of` asks for.
            // place_root_of still answers null (not a frame local); place_outlives_statement
            // answers no (the continuation's temp)
            case NodeType::n_expr_chain_base:
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

bool is_unaccounted_address(const ExprNode &address)
{
    const ExprNode *cur = strip_implicit_casts(&address);

    if (cur == nullptr) {
        return false;
    }

    // **an allow-list: the address of storage reached through a pointer, and nothing else.**
    // a call returning `T&` (`$arr->at(0)`) is a slot its owner still counts. emptying one goes through
    // the owner's own method (`mem::buffer::take`, `put`)
    if (cur->get_node_type() == NodeType::n_expr_addrof) {
        const ExprNode *operand = static_cast<const AddrOfExprNode *>(cur)->operand;
        return operand != nullptr && is_unaccounted_storage(*operand);
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

ChainBaseNode *chain_base_of(ExprNode *expr)
{
    ExprNode *anchor = place_anchor(expr);

    if (anchor == nullptr || anchor->get_node_type() != NodeType::n_expr_chain_base) {
        return nullptr;
    }

    return static_cast<ChainBaseNode *>(anchor);
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

    if (anchor->get_node_type() != NodeType::n_varref) {
        return false;
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
);

// CallResolver's AddrOf(Deref) for a T& that is already an address. peel it
// so `return pass($r)` still sees the borrow local
ExprNode *borrowed_through_deref(ExprNode *argument)
{
    ExprNode *address = strip_implicit_casts(argument);

    if (address == nullptr || address->get_node_type() != NodeType::n_expr_addrof) {
        return address;
    }

    ExprNode *operand = strip_implicit_casts(static_cast<AddrOfExprNode *>(address)->operand);

    if (operand == nullptr || operand->get_node_type() != NodeType::n_expr_deref) {
        return address;
    }

    return static_cast<DerefExprNode *>(operand)->operand;
}

VarDeclNode *names_callee_frame_storage_through_call(
    ExprNode *call,
    FunctionDeclNode *function,
    const std::unordered_set<const VarDeclNode *> &visited
)
{
    const ValueType result = call->result_type();

    if (!result.is_pointer() || result.is_nullable()) {
        return nullptr;
    }

    const FunctionDeclNode *callee = nullptr;
    std::vector<ExprNode *> *arguments = nullptr;

    if (call->get_node_type() == NodeType::n_expr_call) {
        auto *direct = static_cast<FunctionCallExprNode *>(call);
        callee = direct->decl;
        arguments = &direct->arguments;
    }
    else {
        arguments = &static_cast<IndirectCallExprNode *>(call)->arguments;
    }

    const bool yields_mutable = !result.pointee().is_const();
    const bool parameters_align = callee != nullptr && callee->args.size() == arguments->size();

    for (size_t i = 0; i < arguments->size(); i++) {
        if ((*arguments)[i] == nullptr) {
            continue;
        }

        if (yields_mutable && parameters_align && callee->args[i]->has_type()) {
            const ValueType parameter = callee->args[i]->type();

            if (parameter.is_pointer() && parameter.pointee().is_const()) {
                continue;
            }
        }

        std::unordered_set<const VarDeclNode *> argument_visited = visited;

        if (VarDeclNode *root = names_callee_frame_storage_walk(
                borrowed_through_deref((*arguments)[i]), function, argument_visited)) {
            return root;
        }
    }

    return nullptr;
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

    // a match that hands back storage hands back whatever gen_place would address. a payload
    // binding is a T& local, so `$l` and `$l->field` walk the initializer onto the subject;
    // a value local, a field of one, and a by-value parameter are the slot itself. without
    // this arm `return match (make()) { .a($v) => $v }` handed out a dead payload.
    //
    // each arm walks with its own copy of `visited`: every arm reaches the one subject, and a
    // second arm meeting it again is a sibling path rather than a cycle
    if (expr->get_node_type() == NodeType::n_expr_match) {
        auto *match = static_cast<MatchExprNode *>(expr);

        for (const MatchExprNode::Arm &arm : match->arms) {
            if (!match->arm_yields_address(arm) || arm.value == nullptr) {
                continue;
            }

            ExprNode *place = strip_implicit_casts(arm.value);

            if (place == nullptr) {
                continue;
            }

            std::unordered_set<const VarDeclNode *> arm_visited = visited;

            if (is_unaccounted_storage(*place)) {
                continue;
            }

            VarDeclNode *root = place_root_of(place);

            if (root == nullptr) {
                if (VarDeclNode *nested = names_callee_frame_storage_walk(
                        place, function, arm_visited)) {
                    return nested;
                }

                continue;
            }

            if (is_pointer_typed_parameter(root, function)) {
                continue;
            }

            if (is_borrow_local(root, function)) {
                if (!arm_visited.insert(root).second) {
                    return root;
                }

                if (VarDeclNode *from_init = names_callee_frame_storage_walk(
                        root->init_expr, function, arm_visited)) {
                    return from_init;
                }

                continue;
            }

            return root;
        }

        return nullptr;
    }

    if (is_call_expression(*expr)) {
        return names_callee_frame_storage_through_call(expr, function, visited);
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
