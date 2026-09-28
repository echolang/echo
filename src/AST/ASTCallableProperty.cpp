#include "AST/ASTCallableProperty.h"

#include "AST/ASTBundle.h"
#include "AST/ASTDetach.h"
#include "AST/ASTFile.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTRegion.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ScopeNode.h"

namespace AST
{
    namespace
    {
        // the member-call arm of CallResolver::candidates_for, as a filter: unresolved, no
        // namespace, no static owner, a receiver in arguments[0]. one predicate so the rewriter
        // cannot drift from what settle_calls would have looked up as a method
        bool is_unresolved_member_call(const FunctionCallExprNode &call)
        {
            return call.lookup_namespace == nullptr
                && call.decl == nullptr
                && call.static_owner.is_unknown()
                && call.constructed_type.is_unknown()
                && !call.is_shorthand_static_call()
                && !is_print_call(call)
                && !call.arguments.empty()
                && call.arguments[0] != nullptr;
        }

        class CallablePropertyRewriter : public RecursiveVisitor
        {
        public:
            CallablePropertyRewriter(Module &module, DetachBatch &batch) :
                _module(module), _batch(batch)
            {};

            bool changed() const { return _changed; }

            ExprNode *rewrite_value_edge(ExprNode *expr) override
            {
                if (ExprNode *rewritten = rewrite_call(expr)) {
                    return rewritten;
                }

                return RecursiveVisitor::rewrite_value_edge(expr);
            }

            // a call as a statement sits on a scope child, which statement_edge descends into
            // without reseating. the value-edge hook above misses it
            void visitScope(ScopeNode &node) override
            {
                for (size_t i = 0; i < node.children.size(); i++) {
                    NodeReference &child = node.children[i];

                    if (child.has() && child.type() == NodeType::n_expr_call) {
                        if (ExprNode *rewritten = rewrite_call(child.unsafe_ptr<ExprNode>())) {
                            child = make_ref(rewritten);
                        }
                    }

                    statement_edge(child.node());
                }
            }

            void visitFunctionDecl(FunctionDeclNode &node) override
            {
                if (node.is_generic()) {
                    return;
                }

                RecursiveVisitor::visitFunctionDecl(node);
            }

        private:
            ExprNode *rewrite_call(ExprNode *expr)
            {
                if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_call) {
                    return nullptr;
                }

                auto &call = *static_cast<FunctionCallExprNode *>(expr);

                if (!is_unresolved_member_call(call)) {
                    return nullptr;
                }

                ExprNode *recv = call.arguments[0];
                AddrOfExprNode *addrof = nullptr;
                ExprNode *place = recv;

                if (recv->get_node_type() == NodeType::n_expr_addrof) {
                    addrof = static_cast<AddrOfExprNode *>(recv);
                    place = addrof->operand;
                }

                if (place == nullptr) {
                    return nullptr;
                }

                const ValueType receiver = target_type_of(place->result_type());

                if (is_undetermined_type(receiver)
                    || callable_property_of(receiver, call.lookup_name()) == nullptr) {
                    return nullptr;
                }

                if (addrof != nullptr) {
                    addrof->operand = nullptr;
                }

                std::vector<ExprNode *> rest;

                rest.reserve(call.arguments.size() - 1);
                for (size_t i = 1; i < call.arguments.size(); i++) {
                    rest.push_back(call.arguments[i]);
                }

                call.arguments.clear();

                auto &member = _module.nodes.emplace_back<MemberAccessNode>(
                    make_ref(place),
                    call.token_function_name
                );
                auto &indirect = _module.nodes.emplace_back<IndirectCallExprNode>(
                    &member,
                    std::move(rest),
                    call.token_function_name
                );

                if (addrof != nullptr) {
                    _batch.collect(*addrof);
                }

                _batch.collect(call);
                _changed = true;
                return &indirect;
            }

            Module &_module;
            DetachBatch &_batch;
            bool _changed = false;
        };
    }

    bool rewrite_callable_property_calls(Bundle &bundle)
    {
        DetachBatch batch;
        bool changed = false;

        for (auto &module_ptr : bundle.modules) {
            CallablePropertyRewriter rewriter(*module_ptr, batch);

            for_each_semantic_root(
                *module_ptr,
                [&](File &, ScopeNode &root) {
                    root.accept(rewriter);
                }
            );

            changed = changed || rewriter.changed();
        }

        batch.flush(bundle);
        return changed;
    }
};
