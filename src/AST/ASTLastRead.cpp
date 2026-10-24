#include "AST/ASTLastRead.h"

#include "AST/ASTControlFlow.h"
#include "AST/ASTOps.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ExprNode.h"
#include "AST/ForStatementNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/MatchExprNode.h"
#include "AST/OperatorNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"

#include <unordered_map>

namespace
{
    // **where in the body's control flow a node sits**, as the two counts that decide whether a move
    // there would run twice or run on only one of two paths. compared against the declaration's own
    // pair rather than tested for zero: a local declared inside a loop is a fresh value every
    // iteration, so a read of it inside that same loop repeats no more than the declaration does
    struct Position
    {
        size_t loops = 0;
        size_t branches = 0;

        bool operator==(const Position &other) const
        {
            return loops == other.loops && branches == other.branches;
        }
    };

    bool argument_takes_value(const AST::FunctionCallExprNode &node, size_t i)
    {
        if (node.decl == nullptr || i >= node.decl->args.size()) {
            return false;
        }

        const AST::VarDeclNode *param = node.decl->args[i];
        return param != nullptr && param->has_type() && !param->type().is_pointer();
    }

    // one appearance of a declaration in the body. every way a place can name one bottoms out at a
    // VarRefNode - a read, an assignment's target, the operand of an `&`, a member or index base - so
    // that is the whole of what the walk has to record, and the node is kept because it is what
    // AST::OwnershipPass will be holding when it asks
    struct Mention
    {
        const AST::VarDeclNode *decl = nullptr;
        const AST::ExprNode *node = nullptr;
        size_t statement = 0;
        bool returning = false;
        // 0: not in a returning match. sibling arms of one returning match share a group so
        // each arm's last mention of a decl can all be handovers (`return match { => f($x) }`)
        size_t match_group = 0;
        // a by-value destination: the arm value of a `return match`, or a by-value argument.
        // a borrow (`label($sys)`, `$x->tag`) is a last mention that does not consume
        bool consuming = false;
        Position at;
    };

    class MentionCollector : public AST::RecursiveVisitor
    {
    public:
        std::vector<Mention> mentions;

        // where each declaration was written, which is the pair every mention of it is compared with
        std::unordered_map<const AST::VarDeclNode *, Position> declared;

        // declarations a closure captured. the capture is read whenever the callable is, which is a
        // moment no walk of this body can place, so nothing about this declaration's last read is
        // knowable any more
        std::unordered_set<const AST::VarDeclNode *> captured;

        // did any mention at all sit under a `return`? a hand-over is one by definition, so a body
        // where this stayed false has an empty answer and owes none of the work below - which is
        // every void function, every constructor and every file root. a statement-temp mention is
        // the other reason to keep going: `f([1, 2])` on a file root has no return
        bool saw_returning = false;
        bool saw_statement_temp = false;

        // completing arms of a returning match, keyed by match_group. a group hands `$x` over
        // only when every completing arm consumes it; mixed take/read copies, it does not
        // ConditionalMove (libtcp `failure()`)
        std::unordered_map<size_t, size_t> completing_arms;

        void seed_parameter(const AST::VarDeclNode *decl)
        {
            if (decl != nullptr) {
                declared[decl] = Position{};
            }
        }

        // **the statement numbering**, and the reason this arm exists at all: two mentions in one
        // statement are two reads of one declaration with the hand-over between them, so neither is
        // one. restored on the way out, so the statements of a nested scope do not renumber the one
        // they sit inside
        void visitScope(AST::ScopeNode &node) override
        {
            for (size_t i = 0; i < node.children.size(); i++) {
                const size_t enclosing = _statement;
                _statement = ++_statements;

                statement_edge(node.children[i].node());

                _statement = enclosing;
            }
        }

        // the gate: a mention under here is one the enclosing scope ends on
        void visitReturn(AST::ReturnNode &node) override
        {
            const bool enclosing = _returning;
            _returning = true;

            AST::RecursiveVisitor::visitReturn(node);

            _returning = enclosing;
        }

        void visitVarDecl(AST::VarDeclNode &node) override
        {
            declared[&node] = _position;

            AST::RecursiveVisitor::visitVarDecl(node);
        }

        void visitVarRef(AST::VarRefNode &node) override
        {
            if (const AST::VarDeclNode *decl = AST::place_root_of(&node)) {
                mentions.push_back(Mention{
                    decl, &node, _statement, _returning, _match_group, _consuming, _position});
                saw_returning |= _returning;
                saw_statement_temp |= decl->statement_temp;
            }

            AST::RecursiveVisitor::visitVarRef(node);
        }

        AST::ExprNode *rewrite_place_edge(AST::ExprNode *expr) override
        {
            const bool was = _consuming;
            _consuming = false;
            AST::ExprNode *out = AST::RecursiveVisitor::rewrite_place_edge(expr);
            _consuming = was;
            return out;
        }

        void visitFunctionCallExpr(AST::FunctionCallExprNode &node) override
        {
            type_edges(node.explicit_type_args);

            for (size_t i = 0; i < node.arguments.size(); i++) {
                const bool was = _consuming;
                _consuming = argument_takes_value(node, i);
                value_edge(node.arguments[i]);
                _consuming = was;
            }
        }

        void visitWhileStatement(AST::WhileStatementNode &node) override
        {
            // the condition is inside the loop too: it is what every iteration is entered through
            _position.loops++;
            AST::RecursiveVisitor::visitWhileStatement(node);
            _position.loops--;
        }

        void visit_for_statement(AST::ForStatementNode &node) override
        {
            _position.loops++;
            AST::RecursiveVisitor::visit_for_statement(node);
            _position.loops--;
        }

        void visitIfStatement(AST::IfStatementNode &node) override
        {
            value_edge(node.condition);

            walk_arm(node.if_scope);
            walk_arm(node.else_scope);
        }

        void visit_match(AST::MatchExprNode &node) override
        {
            statement_edge(node.subject);

            // a match that *is* the operand of a `return` ends the function on every completing
            // arm, the same way `if { return f($x); }` does. incrementing branches made
            // `return match { => f($x) }` copy. sibling arms share a group so each arm's
            // last mention can all be handovers; a later mention after the match replaces them
            const size_t enclosing_stmt = _statement;
            const size_t group = _returning ? ++_match_groups : 0;
            const size_t saved_group = _match_group;

            for (AST::MatchExprNode::Arm &arm : node.arms) {
                const bool leaves =
                    (arm.scope != nullptr && AST::scope_always_exits(*arm.scope))
                    || (arm.value != nullptr && AST::expression_never_returns(*arm.value));

                if (group != 0 && !leaves) {
                    completing_arms[group]++;
                }

                const bool rejoins = !leaves && !_returning;

                if (rejoins) {
                    _position.branches++;
                }

                _match_group = group;
                _statement = ++_statements;

                statement_edge(arm.scope);

                const bool was_consuming = _consuming;
                _consuming = group != 0;
                value_edge(arm.value);
                _consuming = was_consuming;

                _statement = enclosing_stmt;
                _match_group = saved_group;

                if (rejoins) {
                    _position.branches--;
                }
            }
        }

        void visitBinaryExpr(AST::BinaryExprNode &node) override
        {
            value_edge(node.lhs);

            const Token::Type op = node.op_node->op->type;
            const bool skippable = AST::binary_skips_rhs(op);

            if (skippable) {
                _position.branches++;
            }

            value_edge(node.rhs);

            if (skippable) {
                _position.branches--;
            }
        }

        void visit_null_coalesce(AST::NullCoalesceExprNode &node) override
        {
            value_edge(node.lhs);

            // the present-arm copy and the fallback each run on only one path
            _position.branches++;
            value_edge(node.present_value);
            value_edge(node.rhs);
            _position.branches--;
        }

        void visit_optional_chain(AST::OptionalChainExprNode &node) override
        {
            value_edge(node.base);

            // and the continuation only when it was there
            _position.branches++;
            value_edge(node.continuation);
            _position.branches--;
        }

        void visit_closure_expr(AST::ClosureExprNode &node) override
        {
            for (auto *value : node.captured_values) {
                if (const AST::VarDeclNode *decl = value != nullptr ? AST::place_root_of(value) : nullptr) {
                    captured.insert(decl);
                }
            }

            AST::RecursiveVisitor::visit_closure_expr(node);
        }

        // a nested declaration is its own body, walked as one. descending would number its statements
        // into this body's sequence and record its locals as though they were these
        void visitFunctionDecl(AST::FunctionDeclNode &) override
        {
        }

        // and a type declaration is not flow at all - its properties are ordinary VarDeclNodes, whose
        // initializers belong to the constructor bodies that seat them
        void visit_type_decl(AST::TypeDeclNode &) override
        {
        }

    private:
        // **an arm that always leaves is not a branch for this purpose.** it does not reach the code
        // after the `if`, so what it moved is not visible there and there is no other arm for it to
        // disagree with - which is exactly the reading AST::OwnershipPass's own merge takes. that is
        // what keeps `if (...) { return .ok($out); }` in
        void walk_arm(AST::ScopeNode *arm)
        {
            if (arm == nullptr) {
                return;
            }

            const bool rejoins = !AST::scope_always_exits(*arm);

            if (rejoins) {
                _position.branches++;
            }

            statement_edge(arm);

            if (rejoins) {
                _position.branches--;
            }
        }

        Position _position;

        bool _returning = false;
        bool _consuming = false;

        size_t _statements = 0;
        size_t _statement = 0;

        size_t _match_groups = 0;
        size_t _match_group = 0;
    };
}

std::unordered_set<const AST::ExprNode *> AST::handover_reads_in(
    AST::ScopeNode &body,
    const std::vector<AST::VarDeclNode *> &parameters
)
{
    MentionCollector collector;

    for (const AST::VarDeclNode *parameter : parameters) {
        collector.seed_parameter(parameter);
    }

    body.accept(collector);

    // **nothing under a `return` and no statement-temp means nothing to hand over**, and saying
    // so here is what keeps the two maps below off every body that could never have an answer
    if (!collector.saw_returning && !collector.saw_statement_temp) {
        return {};
    }

    // the last mention of each declaration, and whether anything else shares its statement. one pass
    // over the list, since a later mention simply replaces the candidate an earlier one left
    struct Candidate
    {
        std::vector<size_t> indices;
        bool shared_statement = false;
    };

    std::unordered_map<const AST::VarDeclNode *, Candidate> last;

    for (size_t i = 0; i < collector.mentions.size(); i++) {
        const Mention &mention = collector.mentions[i];

        Candidate &candidate = last[mention.decl];

        if (!candidate.indices.empty()) {
            bool sibling = false;

            for (size_t prev_i : candidate.indices) {
                const Mention &prev = collector.mentions[prev_i];

                if (prev.statement == mention.statement) {
                    candidate.shared_statement = true;
                    sibling = true;
                    break;
                }

                if (prev.match_group != 0 && prev.match_group == mention.match_group) {
                    sibling = true;
                    break;
                }
            }

            if (candidate.shared_statement) {
                continue;
            }

            if (sibling) {
                candidate.indices.push_back(i);
                continue;
            }
        }

        candidate.indices = {i};
        candidate.shared_statement = false;
    }

    std::unordered_set<const AST::ExprNode *> reads;

    for (const auto &[decl, candidate] : last) {
        if (candidate.shared_statement || collector.captured.count(decl) > 0) {
            continue;
        }

        const auto declared = collector.declared.find(decl);

        if (declared == collector.declared.end()) {
            continue;
        }

        const Mention &head = collector.mentions[candidate.indices.front()];

        if (head.match_group != 0) {
            const size_t completing = collector.completing_arms[head.match_group];
            size_t consuming_arms = 0;
            bool all_consume = true;

            for (size_t index : candidate.indices) {
                if (!collector.mentions[index].consuming) {
                    all_consume = false;
                    break;
                }

                consuming_arms++;
            }

            // mixed take/read, or a completing arm that never names the decl: copy, do not
            // ConditionalMove. every completing arm consuming is `return f($x)` / `=> $x`
            if (!all_consume || consuming_arms != completing) {
                continue;
            }
        }

        const bool hoist_temp = decl->statement_temp;

        for (size_t index : candidate.indices) {
            const Mention &mention = collector.mentions[index];

            if (!(mention.at == declared->second)) {
                continue;
            }

            if (hoist_temp || mention.returning) {
                reads.insert(mention.node);
            }
        }
    }

    return reads;
}
