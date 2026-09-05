#include "AST/ASTCastResolution.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCast.h"
#include "AST/ASTCollector.h"
#include "AST/ASTDetach.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTSourceToken.h"
#include "AST/TypeCastNode.h"

namespace AST
{
    CastResolution::CastResolution(Bundle &bundle)
        : FixpointLowering(bundle)
    {
    }

    void CastResolution::refuse(TypeCastNode &node, const TokenReference &at, std::string why)
    {
        _collector.collect_issue<Issue::InvalidTypeConversion>(code_ref_for(at), std::move(why));
        node.plan_decided = true;
    }

    ExprNode *CastResolution::resolve(TypeCastNode &node)
    {
        if (node.expr == nullptr) {
            return &node;
        }

        const CastLookup lookup = cast_plan_for(*node.expr, node.cast_to);

        if (lookup.result == CastLookup::Result::t_pending) {
            // the operand is often an unresolved call, and that call already has a diagnostic of
            // its own. two reports of one failure is worse than one; keep the call's
            if (_finalizing && !_collector.has_critical_issues()) {
                refuse(
                    node,
                    location_of_expression(&node),
                    "this 'as' never got a type, so there is nothing to convert from");
            }

            return &node;
        }

        if (lookup.result == CastLookup::Result::t_refused) {
            refuse(node, location_of_expression(&node), lookup.refusal);
            return &node;
        }

        if (lookup.plan.kind == CastKind::t_declared) {
            // copied before the node leaves the tree: source_token_of falls back through the
            // operand, and forget_subtree is about to take the TypeCastNode out of the arena
            const TokenReference at = location_of_expression(&node);
            const ValueType destination = node.cast_to;
            ExprNode *operand = node.expr;
            node.expr = nullptr;
            forget_subtree(_bundle, node);

            _changed = true;
            return emit_declared_conversion(
                _current_module->nodes, operand, lookup.plan.decl, destination, at);
        }

        node.plan_decided = true;
        return &node;
    }

    ExprNode *CastResolution::rewrite_value_edge(ExprNode *expr)
    {
        ExprNode *walked = RecursiveVisitor::rewrite_value_edge(expr);

        if (walked == nullptr || walked->get_node_type() != NodeType::n_type_cast) {
            return walked;
        }

        auto &cast = static_cast<TypeCastNode &>(*walked);

        if (cast.is_implcit || cast.plan_decided) {
            return walked;
        }

        return resolve(cast);
    }
};
