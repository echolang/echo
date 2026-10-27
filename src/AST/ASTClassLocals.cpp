#include "AST/ASTClassLocals.h"

#include "AST/ASTCollector.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTRegion.h"
#include "AST/AssignNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ExprNode.h"
#include "AST/ForStatementNode.h"
#include "AST/ForeachNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/GuardNode.h"
#include "AST/IfStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/MatchExprNode.h"
#include "AST/ReleaseNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"

#include <fmt/core.h>

#include <unordered_set>
#include <utility>

namespace
{
    typedef std::unordered_set<const AST::VarDeclNode *> UninitSet;

    struct UninitPath
    {
        UninitSet fallthrough;
        bool falls_through = true;
    };

    AST::CodeRef code_ref(AST::Module &module, const TokenReference &token)
    {
        return AST::CodeRef { &module, token.make_slice() };
    }

    bool tracks_class_local(const AST::VarDeclNode &node)
    {
        if (node.init_expr != nullptr || !node.has_type()) {
            return false;
        }

        if (!node.type().is_class() || node.type().is_nullable()) {
            return false;
        }

        if (node.token_varname.value() == "$this") {
            return false;
        }

        AST::ComplexType *ct = node.type().get_complex_type();
        const bool bare_generic = ct != nullptr && ct->is_generic() && !ct->is_instantiated();
        return !bare_generic;
    }

    UninitPath join_uninit(
        const UninitPath &then_r,
        const UninitPath *else_r,
        const UninitSet &incoming
    )
    {
        UninitPath result;
        result.fallthrough = incoming;

        if (else_r == nullptr) {
            if (then_r.falls_through) {
                result.fallthrough.insert(
                    then_r.fallthrough.begin(), then_r.fallthrough.end());
            }
            return result;
        }

        if (then_r.falls_through && else_r->falls_through) {
            result.fallthrough = then_r.fallthrough;
            result.fallthrough.insert(
                else_r->fallthrough.begin(), else_r->fallthrough.end());
        }
        else if (then_r.falls_through) {
            result.fallthrough = then_r.fallthrough;
        }
        else if (else_r->falls_through) {
            result.fallthrough = else_r->fallthrough;
        }
        else {
            result.falls_through = false;
        }

        return result;
    }

    class ClassLocalWalk : public AST::RecursiveVisitor
    {
    public:
        UninitPath result;
        AST::Collector &collector;
        AST::Module &module;
        bool in_release = false;
        bool as_statement = false;

        ClassLocalWalk(UninitSet incoming, AST::Collector &collector, AST::Module &module) :
            result { std::move(incoming), true },
            collector(collector),
            module(module)
        {}

        void statement_edge(AST::Node *node) override
        {
            const bool previous = as_statement;
            as_statement = true;
            AST::RecursiveVisitor::statement_edge(node);
            as_statement = previous;
        }

        void visitFunctionDecl(AST::FunctionDeclNode &) override {}
        void visit_type_decl(AST::TypeDeclNode &) override {}

        void visit_closure_expr(AST::ClosureExprNode &node) override
        {
            const UninitSet prev = result.fallthrough;
            AST::RecursiveVisitor::visit_closure_expr(node);
            result.fallthrough = prev;
        }

        void visitVarDecl(AST::VarDeclNode &node) override
        {
            collect_value(node.init_expr);

            if (tracks_class_local(node)) {
                result.fallthrough.insert(&node);
            }
        }

        void visitVarRef(AST::VarRefNode &node) override
        {
            if (!in_release && node.is_var()) {
                AST::VarDeclNode &decl = node.get_var().decl();

                if (result.fallthrough.count(&decl) > 0) {
                    collector.collect_issue<AST::Issue::GenericError>(
                        code_ref(module, node.get_var().use_token()),
                        fmt::format(
                            "'{}' has type '{}' which owns a resource, so it needs an initializer",
                            decl.name_full(),
                            decl.type().get_type_desciption()));
                    result.fallthrough.erase(&decl);
                }
            }

            AST::RecursiveVisitor::visitVarRef(node);
        }

        void visit_release(AST::ReleaseNode &node) override
        {
            const bool prev = in_release;
            in_release = true;
            AST::RecursiveVisitor::visit_release(node);
            in_release = prev;
        }

        void visit_assign(AST::AssignNode &node) override
        {
            statement_edge(node.target_bind);
            collect_value(node.value_expr);

            AST::VarDeclNode *seated = nullptr;
            if (node.target != nullptr) {
                AST::ExprNode *place = AST::strip_implicit_casts(node.target);
                if (place != nullptr && place->get_node_type() == AST::NodeType::n_varref) {
                    seated = AST::place_root_of(place);
                }
            }

            if (seated != nullptr) {
                result.fallthrough.erase(seated);
            }

            collect_value(node.target);
            statement_edge(node.teardown_old);
        }

        void visitScope(AST::ScopeNode &node) override
        {
            UninitSet current = result.fallthrough;

            for (const AST::NodeReference &child : node.children) {
                UninitPath step = walk_statement(child.node(), current);
                if (!step.falls_through) {
                    result.falls_through = false;
                    result.fallthrough = std::move(step.fallthrough);
                    return;
                }

                current = std::move(step.fallthrough);
            }

            result.fallthrough = std::move(current);
        }

        void visitReturn(AST::ReturnNode &node) override
        {
            collect_value(node.expr);
            statement_edges(node.unwind);
            result.falls_through = false;
        }

        void visit_loop_control(AST::LoopControlNode &) override
        {
            result.falls_through = false;
        }

        void visitFunctionCallExpr(AST::FunctionCallExprNode &node) override
        {
            AST::RecursiveVisitor::visitFunctionCallExpr(node);

            if (as_statement && AST::expression_never_returns(node)) {
                result.falls_through = false;
            }
        }

        void visitIfStatement(AST::IfStatementNode &node) override
        {
            join_if(node.condition, node.if_scope, node.else_scope);
        }

        void visit_const_if(AST::ConstIfNode &node) override
        {
            join_if(node.condition, node.if_scope, node.else_scope);
        }

        void visit_guard(AST::GuardNode &node) override
        {
            const UninitSet incoming = result.fallthrough;
            collect_node(node.decl);
            collect_value(node.subject);
            collect_value(node.presence_test);
            collect_value(node.bound_value);
            const UninitSet after_success = result.fallthrough;

            UninitPath else_r = walk_statement(node.else_scope, incoming);
            if (else_r.falls_through) {
                UninitSet joined = after_success;
                joined.insert(else_r.fallthrough.begin(), else_r.fallthrough.end());
                result.fallthrough = std::move(joined);
            }
            else {
                result.fallthrough = after_success;
            }
        }

        void visit_match(AST::MatchExprNode &node) override
        {
            const UninitSet incoming = result.fallthrough;
            collect_node(node.subject);

            bool any_falls = false;
            bool first = true;
            UninitSet joined;

            for (const AST::MatchExprNode::Arm &arm : node.arms) {
                UninitPath arm_r = walk_statement(arm.scope, incoming);
                collect_value_with(arm.value, arm_r.fallthrough);

                const bool arm_dies = arm.value != nullptr
                    && AST::expression_never_returns(*arm.value);
                if (arm_dies || !arm_r.falls_through) {
                    continue;
                }

                any_falls = true;
                if (first) {
                    joined = std::move(arm_r.fallthrough);
                    first = false;
                }
                else {
                    joined.insert(arm_r.fallthrough.begin(), arm_r.fallthrough.end());
                }
            }

            if (!any_falls) {
                result.falls_through = false;
                return;
            }

            result.fallthrough = std::move(joined);
        }

        void visitWhileStatement(AST::WhileStatementNode &node) override
        {
            const UninitSet incoming = result.fallthrough;
            collect_value(node.condition);
            UninitPath body_r = walk_statement(node.loop_scope, incoming);
            result.fallthrough = incoming;
            result.fallthrough.insert(body_r.fallthrough.begin(), body_r.fallthrough.end());
        }

        void visit_for_statement(AST::ForStatementNode &node) override
        {
            const UninitSet incoming = result.fallthrough;
            collect_value(node.condition);
            UninitPath body_r = walk_statement(node.loop_scope, incoming);
            UninitPath step_r = walk_statement(node.step, incoming);
            result.fallthrough = incoming;
            result.fallthrough.insert(body_r.fallthrough.begin(), body_r.fallthrough.end());
            result.fallthrough.insert(step_r.fallthrough.begin(), step_r.fallthrough.end());
        }

        void visit_foreach(AST::ForeachNode &node) override
        {
            const UninitSet incoming = result.fallthrough;
            collect_value(node.source);
            UninitPath body_r = walk_statement(node.body, incoming);
            result.fallthrough = incoming;
            result.fallthrough.insert(body_r.fallthrough.begin(), body_r.fallthrough.end());
        }

    private:
        void collect_value(AST::ExprNode *expr)
        {
            collect_node(expr);
        }

        void collect_node(AST::Node *node)
        {
            if (node == nullptr) {
                return;
            }

            const bool previous = as_statement;
            as_statement = false;
            node->accept(*this);
            as_statement = previous;
        }

        void collect_value_with(AST::ExprNode *expr, const UninitSet &uninitialized)
        {
            if (expr == nullptr) {
                return;
            }

            const UninitSet saved = result.fallthrough;
            result.fallthrough = uninitialized;
            collect_value(expr);
            result.fallthrough = saved;
        }

        void join_if(
            AST::ExprNode *condition,
            AST::ScopeNode *then_scope,
            AST::ScopeNode *else_scope
        )
        {
            const UninitSet incoming = result.fallthrough;
            collect_value(condition);
            UninitPath then_r = walk_statement(then_scope, incoming);
            if (else_scope == nullptr) {
                result = join_uninit(then_r, nullptr, incoming);
                return;
            }

            UninitPath else_r = walk_statement(else_scope, incoming);
            result = join_uninit(then_r, &else_r, incoming);
        }

        UninitPath walk_statement(AST::Node *node, UninitSet incoming)
        {
            ClassLocalWalk inner(std::move(incoming), collector, module);
            inner.statement_edge(node);
            return inner.result;
        }
    };
}

void AST::check_class_locals(Module &module, Collector &collector)
{
    for_each_semantic_root(module, [&](File &, ScopeNode &root) {
        ClassLocalWalk walk({}, collector, module);
        walk.visitScope(root);
    });

    for (FunctionDeclNode *fn : module.nodes.of_type<FunctionDeclNode>()) {
        if (fn == nullptr || fn->is_generic() || fn->body == nullptr) {
            continue;
        }

        if (fn->has_synthesized_body()) {
            continue;
        }

        ClassLocalWalk walk({}, collector, module);
        walk.statement_edge(fn->body);
    }
}
