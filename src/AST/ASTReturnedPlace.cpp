#include "AST/ASTReturnedPlace.h"

#include "AST/ASTControlFlow.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTRegion.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MatchExprNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"

#include <optional>
#include <unordered_set>
#include <vector>

namespace AST
{

namespace
{

    // **total by construction**, so a NodeType added without an arm is still walked.
    // nested decls are their own bodies. a missed return of this one is fail-open
    class CollectReturns : public RecursiveVisitor
    {
    public:
        std::vector<ExprNode *> &out;

        explicit CollectReturns(std::vector<ExprNode *> &into) : out(into) {}

        void visitFunctionDecl(FunctionDeclNode &) override
        {
        }

        void visit_type_decl(TypeDeclNode &) override
        {
        }

        void visitReturn(ReturnNode &node) override
        {
            if (node.expr != nullptr && !expression_never_returns(*node.expr)) {
                out.push_back(node.expr);
            }

            RecursiveVisitor::visitReturn(node);
        }
    };

    std::optional<std::vector<std::string>> place_named_by(
        ExprNode *expr,
        VarDeclNode *self,
        const FunctionDeclNode *fn,
        std::unordered_set<const VarDeclNode *> &visited
    );

    std::optional<std::vector<std::string>> match_place(
        MatchExprNode &match,
        VarDeclNode *self,
        const FunctionDeclNode *fn,
        const std::unordered_set<const VarDeclNode *> &visited
    )
    {
        if (!match.patterns_decided) {
            return std::nullopt;
        }

        std::optional<std::vector<std::string>> agreed;

        for (MatchExprNode::Arm &arm : match.arms) {
            if (arm.scope != nullptr && scope_always_exits(*arm.scope)) {
                continue;
            }

            if (arm.value == nullptr || expression_never_returns(*arm.value)
                || expression_produces_no_value(*arm.value)) {
                continue;
            }

            // each arm reaches the one subject; a second meeting it is a sibling, not a cycle
            std::unordered_set<const VarDeclNode *> arm_visited = visited;
            auto path = place_named_by(arm.value, self, fn, arm_visited);

            if (!path.has_value()) {
                return std::nullopt;
            }

            if (agreed.has_value() && *agreed != *path) {
                return std::nullopt;
            }

            agreed = std::move(path);
        }

        return agreed;
    }

    std::optional<std::vector<std::string>> place_named_by(
        ExprNode *expr,
        VarDeclNode *self,
        const FunctionDeclNode *fn,
        std::unordered_set<const VarDeclNode *> &visited
    )
    {
        expr = strip_implicit_casts(expr);

        if (expr == nullptr || self == nullptr) {
            return std::nullopt;
        }

        switch (expr->get_node_type()) {
            case NodeType::n_expr_addrof:
                return place_named_by(static_cast<AddrOfExprNode *>(expr)->operand, self, fn, visited);

            case NodeType::n_expr_deref:
                return place_named_by(static_cast<DerefExprNode *>(expr)->operand, self, fn, visited);

            case NodeType::n_varref:
            {
                VarDeclNode *root = place_root_of(expr);

                if (root == self) {
                    return std::vector<std::string>{};
                }

                // a match payload binding is `&$__match->__cN_field`; only a T& local
                // is that projection. a by-value copy of `$this` names the copy
                if (is_borrow_local(root, fn) && root->init_expr != nullptr) {
                    if (!visited.insert(root).second) {
                        return std::nullopt;
                    }

                    return place_named_by(root->init_expr, self, fn, visited);
                }

                return std::nullopt;
            }

            case NodeType::n_member_access:
            {
                auto *access = static_cast<MemberAccessNode *>(expr);
                auto &base = access->get_base_node();

                if (!base.has() || !base.is_expression_node()) {
                    return std::nullopt;
                }

                auto inner = place_named_by(base.unsafe_ptr<ExprNode>(), self, fn, visited);

                if (!inner.has_value()) {
                    return std::nullopt;
                }

                inner->push_back(access->get_member_name().value());
                return inner;
            }

            case NodeType::n_expr_match:
                return match_place(*static_cast<MatchExprNode *>(expr), self, fn, visited);

            default:
                return std::nullopt;
        }
    }

    void append_path(std::vector<std::string> &into, const std::vector<std::string> &extra)
    {
        into.insert(into.end(), extra.begin(), extra.end());
    }

    bool remainder_of_properties(
        const ComplexType *ct,
        size_t skip_index,
        const std::vector<std::string> &rest,
        size_t first,
        size_t count
    )
    {
        for (size_t i = 0; i < count; i++) {
            const size_t index = first + i;
            const ComplexType::Property &prop = ct->get_property(index);

            if (index == skip_index) {
                if (!rest.empty() && remainder_needs_destruction(prop.type, rest)) {
                    return true;
                }

                continue;
            }

            if (needs_destruction(prop.type)) {
                return true;
            }
        }

        return false;
    }

};

BorrowResult borrow_result_of(const FunctionDeclNode *fn)
{
    BorrowResult none;

    if (fn == nullptr || !fn->has_receiver() || fn->body == nullptr || fn->args.empty()
        || fn->args[0] == nullptr) {
        return none;
    }

    if (!fn->get_return_type().is_pointer()) {
        return none;
    }

    if (!body_is_concrete(*fn->body)) {
        return none;
    }

    std::vector<ExprNode *> returns;
    CollectReturns walk(returns);
    fn->body->accept(walk);

    if (returns.empty()) {
        return none;
    }

    VarDeclNode *self = fn->args[0];
    std::optional<std::vector<std::string>> agreed;

    for (ExprNode *expr : returns) {
        std::unordered_set<const VarDeclNode *> visited;
        auto path = place_named_by(expr, self, fn, visited);

        if (!path.has_value()) {
            return none;
        }

        if (agreed.has_value() && *agreed != *path) {
            return none;
        }

        agreed = std::move(path);
    }

    BorrowResult result;
    result.path = std::move(*agreed);
    result.kind = result.path.empty() ? BorrowResultKind::t_whole : BorrowResultKind::t_field;
    return result;
}

std::optional<BorrowChain> borrow_chain_of(ExprNode *expr)
{
    expr = strip_implicit_casts(expr);

    if (expr == nullptr) {
        return std::nullopt;
    }

    switch (expr->get_node_type()) {
        case NodeType::n_expr_deref:
            return borrow_chain_of(static_cast<DerefExprNode *>(expr)->operand);

        case NodeType::n_expr_addrof:
            return borrow_chain_of(static_cast<AddrOfExprNode *>(expr)->operand);

        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(expr);

            if (call->decl == nullptr || !call->result_type().is_pointer()) {
                BorrowChain chain;
                chain.root = expr;
                return chain;
            }

            const BorrowResult named = borrow_result_of(call->decl);

            if (named.kind == BorrowResultKind::t_none || call->arguments.empty()
                || call->arguments[0] == nullptr) {
                return std::nullopt;
            }

            auto inner = borrow_chain_of(call->arguments[0]);

            if (!inner.has_value()) {
                return std::nullopt;
            }

            append_path(inner->path, named.path);
            return inner;
        }

        default:
        {
            BorrowChain chain;
            chain.root = expr;
            return chain;
        }
    }
}

bool remainder_needs_destruction(const ValueType &holder, const std::vector<std::string> &path)
{
    if (path.empty()) {
        return false;
    }

    if (!holder.has_complex_type()) {
        return true;
    }

    const ComplexType *ct = holder.get_complex_type();

    // a class always owes a release; a user destructor is teardown leftover fields
    // do not describe. taking one field of a dying Holder still runs it
    if (holder.is_class() || holder.is_interface() || find_destructor(ct) != nullptr) {
        return true;
    }

    const ComplexType::Property *prop = ct->find_property(path[0]);

    if (prop == nullptr) {
        return true;
    }

    const std::vector<std::string> rest(path.begin() + 1, path.end());

    if (ct->is_optional) {
        if (path[0] != k_optional_value_name) {
            return true;
        }

        return remainder_of_properties(ct, prop->index, rest, 0, ct->property_count());
    }

    if (ct->is_enum_kind()) {
        for (const ComplexType::EnumCase &entry : ct->enum_cases()) {
            if (prop->index >= entry.first_payload_property
                && prop->index < entry.first_payload_property + entry.payload_field_count) {
                return remainder_of_properties(
                    ct,
                    prop->index,
                    rest,
                    entry.first_payload_property,
                    entry.payload_field_count);
            }
        }

        return true;
    }

    return remainder_of_properties(ct, prop->index, rest, 0, ct->property_count());
}

};
