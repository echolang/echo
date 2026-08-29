#include "AST/ASTConstruction.h"

#include "AST/ASTCollector.h"
#include "AST/ASTConstructor.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTIssue.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTSourceToken.h"
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
#include "AST/MemberAccessNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/WhileStatementNode.h"

#include <fmt/core.h>

#include <unordered_set>
#include <vector>

namespace
{
    typedef std::unordered_set<std::string> FieldSet;

    struct PathResult
    {
        FieldSet fallthrough;
        bool falls_through = true;
        std::vector<FieldSet> completed;
    };

    struct Read
    {
        std::string name;
        TokenReference at;
    };

    AST::ExprNode *peel_address(AST::ExprNode *expr)
    {
        while (expr != nullptr) {
            const AST::NodeType kind = expr->get_node_type();
            if (kind == AST::NodeType::n_expr_peel) {
                expr = static_cast<AST::PointerValueNode *>(expr)->operand;
            }
            else if (kind == AST::NodeType::n_expr_deref) {
                expr = static_cast<AST::DerefExprNode *>(expr)->operand;
            }
            else {
                break;
            }
        }

        return expr;
    }

    // `$this->field`, not `$this->field->next`. the outer access of a nested path is a different
    // object's member; RecursiveVisitor still visits the inner `$this->field` as a read
    const std::string *direct_property_of_self(AST::ExprNode *expr, const AST::VarDeclNode *self)
    {
        expr = peel_address(expr);

        if (expr == nullptr || expr->get_node_type() != AST::NodeType::n_member_access) {
            return nullptr;
        }

        if (AST::place_root_of(expr) != self) {
            return nullptr;
        }

        auto *access = static_cast<AST::MemberAccessNode *>(expr);
        AST::NodeReference &base_ref = access->get_base_node();
        AST::ExprNode *base = base_ref.has() && base_ref.is_expression_node()
            ? peel_address(base_ref.unsafe_ptr<AST::ExprNode>())
            : nullptr;

        if (base != nullptr && base->get_node_type() == AST::NodeType::n_member_access) {
            return nullptr;
        }

        return &access->get_member_name().value();
    }

    FieldSet intersect(const FieldSet &a, const FieldSet &b)
    {
        FieldSet out;

        for (const std::string &name : a) {
            if (b.count(name) != 0) {
                out.insert(name);
            }
        }

        return out;
    }

    class AssignCollector : public AST::RecursiveVisitor
    {
    public:
        AssignCollector(const AST::VarDeclNode *self, FieldSet *into) : _self(self), _into(into) {}

        void visitFunctionDecl(AST::FunctionDeclNode &) override {}

        void visit_assign(AST::AssignNode &node) override
        {
            if (const std::string *name = direct_property_of_self(node.target, _self)) {
                _into->insert(*name);
            }

            AST::RecursiveVisitor::visit_assign(node);
        }

    private:
        const AST::VarDeclNode *_self;
        FieldSet *_into;
    };

    void join_completed(PathResult &into, const PathResult &from)
    {
        into.completed.insert(into.completed.end(), from.completed.begin(), from.completed.end());
    }

    PathResult join_branches(
        const PathResult &then_r,
        const PathResult *else_r,
        const FieldSet &incoming
    )
    {
        PathResult result;
        result.fallthrough = incoming;
        join_completed(result, then_r);

        if (else_r == nullptr) {
            if (then_r.falls_through) {
                result.fallthrough = intersect(then_r.fallthrough, incoming);
            }
            return result;
        }

        join_completed(result, *else_r);

        if (then_r.falls_through && else_r->falls_through) {
            result.fallthrough = intersect(then_r.fallthrough, else_r->fallthrough);
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

    // path-sensitive definite assignment. control-flow nodes are the overrides; everything else
    // is RecursiveVisitor descent that records `$this->field` reads. a new leaf kind therefore
    // collects reads for free, which is the reason this is not a switch over NodeType
    class ConstructionWalk : public AST::RecursiveVisitor
    {
    public:
        PathResult result;
        const AST::VarDeclNode *self = nullptr;
        std::vector<Read> *reads = nullptr;
        bool as_statement = false;

        ConstructionWalk(FieldSet incoming, const AST::VarDeclNode *self, std::vector<Read> *reads) :
            result { std::move(incoming), true, {} }, self(self), reads(reads)
        {}

        void statement_edge(AST::Node *node) override
        {
            const bool previous = as_statement;
            as_statement = true;
            AST::RecursiveVisitor::statement_edge(node);
            as_statement = previous;
        }

        void visitFunctionDecl(AST::FunctionDeclNode &) override {}

        void visitMemberAccess(AST::MemberAccessNode &node) override
        {
            if (reads != nullptr) {
                if (const std::string *name = direct_property_of_self(&node, self)) {
                    if (result.fallthrough.count(*name) == 0) {
                        const TokenReference *token = AST::source_token_of(node);
                        if (token != nullptr) {
                            reads->push_back(Read { *name, *token });
                        }
                    }
                }
            }

            AST::RecursiveVisitor::visitMemberAccess(node);
        }

        void visitScope(AST::ScopeNode &node) override
        {
            FieldSet current = result.fallthrough;

            for (const AST::NodeReference &child : node.children) {
                PathResult step = walk_statement(child.node(), current);
                join_completed(result, step);

                if (!step.falls_through) {
                    result.falls_through = false;
                    return;
                }

                current = std::move(step.fallthrough);
            }

            result.fallthrough = std::move(current);
        }

        void visitReturn(AST::ReturnNode &node) override
        {
            collect_value(node.expr);
            result.completed.push_back(result.fallthrough);
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

        void visit_assign(AST::AssignNode &node) override
        {
            collect_value(node.value_expr);

            if (const std::string *name = direct_property_of_self(node.target, self)) {
                result.fallthrough.insert(*name);
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
            collect_node(node.decl);
            collect_value(node.presence_test);
            collect_value(node.bound_value);

            PathResult else_r = walk_statement(node.else_scope, result.fallthrough);
            join_completed(result, else_r);
        }

        void visit_match(AST::MatchExprNode &node) override
        {
            const FieldSet incoming = result.fallthrough;
            collect_node(node.subject);

            bool any_falls = false;
            bool first = true;
            FieldSet joined;

            for (const AST::MatchExprNode::Arm &arm : node.arms) {
                PathResult arm_r = walk_statement(arm.scope, incoming);
                collect_value_with(arm.value, arm_r.fallthrough);
                join_completed(result, arm_r);

                const bool arm_dies = arm.value != nullptr && AST::expression_never_returns(*arm.value);
                if (arm_dies || !arm_r.falls_through) {
                    continue;
                }

                any_falls = true;
                if (first) {
                    joined = arm_r.fallthrough;
                    first = false;
                }
                else {
                    joined = intersect(joined, arm_r.fallthrough);
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
            collect_value(node.condition);
            PathResult body_r = walk_statement(node.loop_scope, result.fallthrough);
            join_completed(result, body_r);
        }

        void visit_for_statement(AST::ForStatementNode &node) override
        {
            const FieldSet incoming = result.fallthrough;
            collect_value(node.condition);
            PathResult body_r = walk_statement(node.loop_scope, incoming);
            join_completed(result, body_r);
            PathResult step_r = walk_statement(node.step, incoming);
            join_completed(result, step_r);
        }

        void visit_foreach(AST::ForeachNode &node) override
        {
            collect_value(node.source);
            PathResult body_r = walk_statement(node.body, result.fallthrough);
            join_completed(result, body_r);
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

        void collect_value_with(AST::ExprNode *expr, const FieldSet &assigned)
        {
            if (expr == nullptr) {
                return;
            }

            const FieldSet saved = result.fallthrough;
            result.fallthrough = assigned;
            collect_value(expr);
            result.fallthrough = saved;
        }

        void join_if(AST::ExprNode *condition, AST::ScopeNode *then_scope, AST::ScopeNode *else_scope)
        {
            const FieldSet incoming = result.fallthrough;
            collect_value(condition);
            PathResult then_r = walk_statement(then_scope, incoming);
            if (else_scope == nullptr) {
                result = join_branches(then_r, nullptr, incoming);
                return;
            }

            PathResult else_r = walk_statement(else_scope, incoming);
            result = join_branches(then_r, &else_r, incoming);
        }

        PathResult walk_statement(AST::Node *node, FieldSet incoming)
        {
            ConstructionWalk inner(std::move(incoming), self, reads);
            inner.statement_edge(node);
            return inner.result;
        }
    };

    PathResult walk_scope(
        const AST::ScopeNode *scope,
        FieldSet incoming,
        const AST::VarDeclNode *self,
        std::vector<Read> *reads
    )
    {
        if (scope == nullptr) {
            PathResult result;
            result.fallthrough = std::move(incoming);
            return result;
        }

        ConstructionWalk walk(std::move(incoming), self, reads);
        walk.statement_edge(const_cast<AST::ScopeNode *>(scope));
        return walk.result;
    }

    const AST::VarDeclNode *init_this(const AST::FunctionDeclNode &init)
    {
        return init.args.empty() ? nullptr : init.args[0];
    }

    AST::CodeRef code_ref(AST::Module &module, const TokenReference &token)
    {
        return AST::CodeRef { &module, token.make_slice() };
    }
}

std::unordered_set<std::string> AST::fields_assigned_on_all_paths(
    const AST::ScopeNode *body,
    const AST::VarDeclNode *self
)
{
    if (body == nullptr || self == nullptr) {
        return {};
    }

    const PathResult walked = walk_scope(body, {}, self, nullptr);
    std::vector<FieldSet> completing = walked.completed;

    if (walked.falls_through) {
        completing.push_back(walked.fallthrough);
    }

    if (completing.empty()) {
        return {};
    }

    FieldSet all = completing.front();
    for (size_t i = 1; i < completing.size(); i++) {
        all = intersect(all, completing[i]);
    }

    return all;
}

std::unordered_set<std::string> AST::derived_fields_of(const AST::TypeDeclNode &type)
{
    AST::FunctionDeclNode *init = type.complex_type().type_init();
    if (init == nullptr) {
        return {};
    }

    return fields_assigned_on_all_paths(init->body, init_this(*init));
}

void AST::check_construction(AST::TypeDeclNode &type, AST::Collector &collector, AST::Module &module)
{
    AST::FunctionDeclNode *init = type.complex_type().type_init();
    const FieldSet derived = derived_fields_of(type);

    auto *init_self = init == nullptr ? nullptr : init_this(*init);

    if (init != nullptr && init->body != nullptr && init_self != nullptr) {
        FieldSet any_in_init;
        AssignCollector any_collector(init_self, &any_in_init);
        init->body->accept(any_collector);

        for (const std::string &name : any_in_init) {
            if (derived.count(name) != 0) {
                continue;
            }

            AST::VarDeclNode *prop = nullptr;
            for (AST::VarDeclNode *candidate : type.properties()) {
                if (candidate != nullptr && candidate->name() == name) {
                    prop = candidate;
                    break;
                }
            }

            if (prop == nullptr) {
                continue;
            }

            collector.collect_issue<AST::Issue::InitAssignsOnSomePaths>(
                code_ref(module, prop->token_varname),
                fmt::format(
                    "'{}' is not assigned on all paths of 'init'. Give every path an assignment, "
                    "or leave it to the constructor.",
                    prop->name_full()));
        }

        FieldSet ctor_entry;

        auto note_ctor_assigns = [&](AST::FunctionDeclNode *ctor) {
            if (ctor == nullptr || ctor->body == nullptr) {
                return;
            }

            const AST::VarDeclNode *self = AST::constructor_this(*ctor);
            FieldSet assigned = fields_assigned_on_all_paths(ctor->body, self);

            if (ctor_entry.empty() && type.constructors().size() + (type.synthesized_constructor() != nullptr ? 1 : 0) == 1) {
                ctor_entry = assigned;
                return;
            }

            if (ctor_entry.empty()) {
                ctor_entry = assigned;
            }
            else {
                ctor_entry = intersect(ctor_entry, assigned);
            }
        };

        for (AST::FunctionDeclNode *ctor : type.constructors()) {
            note_ctor_assigns(ctor);
        }

        note_ctor_assigns(type.synthesized_constructor());

        std::vector<Read> reads;
        walk_scope(init->body, ctor_entry, init_self, &reads);

        for (const Read &read : reads) {
            collector.collect_issue<AST::Issue::InitReadsUnassignedField>(
                code_ref(module, read.at),
                fmt::format(
                    "'init' reads '{}' before every constructor has assigned it.",
                    read.name));
        }

    }

    auto check_ctor = [&](AST::FunctionDeclNode *ctor) {
        if (ctor == nullptr || ctor->body == nullptr) {
            return;
        }

        const AST::VarDeclNode *self = AST::constructor_this(*ctor);
        const FieldSet assigned = fields_assigned_on_all_paths(ctor->body, self);

        for (AST::VarDeclNode *prop : type.properties()) {
            if (prop == nullptr || prop->is_static()) {
                continue;
            }

            if (derived.count(prop->name()) != 0) {
                continue;
            }

            // a class handle, a pointer, a weak or a C function pointer is valid at zero: the
            // constructor's `$this` slot is zero-filled, and that word *is* null. requiring an
            // assignment would force `$this->owner = null` on a non-nullable `str::buf`, which
            // the type refuses. a struct, an enum and a primitive have no such empty word
            if (prop->has_type() && prop->type().has_null_representation()) {
                continue;
            }

            if (assigned.count(prop->name()) != 0) {
                continue;
            }

            collector.collect_issue<AST::Issue::ConstructionLeavesFieldUnassigned>(
                code_ref(module, ctor->declaration_site_token()),
                fmt::format(
                    "'{}' is not assigned on all paths of this constructor.",
                    prop->name_full()));
        }
    };

    for (AST::FunctionDeclNode *ctor : type.constructors()) {
        check_ctor(ctor);
    }

    check_ctor(type.synthesized_constructor());
}
