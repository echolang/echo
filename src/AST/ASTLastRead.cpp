#include "AST/ASTLastRead.h"

#include "AST/ASTControlFlow.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ExprNode.h"
#include "AST/ForStatementNode.h"
#include "AST/IfStatementNode.h"
#include "AST/MatchExprNode.h"
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
        // every void function, every constructor and every file root
        bool saw_returning = false;

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
                mentions.push_back(Mention{decl, &node, _statement, _returning, _position});
                saw_returning |= _returning;
            }

            AST::RecursiveVisitor::visitVarRef(node);
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

            for (AST::MatchExprNode::Arm &arm : node.arms) {
                // **the same walk_arm an `if` gets.** an arm that always leaves does not rejoin,
                // and a match that *is* the operand of a `return` does not rejoin with later uses
                // of a local either - the function is over. incrementing unconditionally made
                // `return match { => .ok($out) }` copy where `if { return .ok($out); }` handed over
                const bool leaves =
                    (arm.scope != nullptr && AST::scope_always_exits(*arm.scope))
                    || (arm.value != nullptr && AST::expression_never_returns(*arm.value));

                // `_returning` is not a reason to skip the increment: a match used as a
                // return value still joins its arms at a phi, and handing over on one arm
                // only is the conditional move OwnershipPass reports. skip only when the
                // arm itself always leaves, the same walk_arm an `if` gets
                const bool rejoins = !leaves;

                if (rejoins) {
                    _position.branches++;
                }

                statement_edge(arm.scope);
                value_edge(arm.value);

                if (rejoins) {
                    _position.branches--;
                }
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

        size_t _statements = 0;
        size_t _statement = 0;
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

    // **nothing under a `return` means nothing to hand over**, and saying so here is what keeps the two
    // maps below off every body that could never have an answer
    if (!collector.saw_returning) {
        return {};
    }

    // the last mention of each declaration, and whether anything else shares its statement. one pass
    // over the list, since a later mention simply replaces the candidate an earlier one left
    struct Candidate
    {
        size_t index = 0;
        bool seen = false;
        bool shared_statement = false;
    };

    std::unordered_map<const AST::VarDeclNode *, Candidate> last;

    for (size_t i = 0; i < collector.mentions.size(); i++) {
        const Mention &mention = collector.mentions[i];

        Candidate &candidate = last[mention.decl];

        // a second mention in the statement the candidate sits in **disqualifies that candidate**
        // rather than replacing it, which is the whole of the aliasing rule: a statement naming the
        // declaration twice has one of the two handing the value over while the other still reads it
        if (candidate.seen && collector.mentions[candidate.index].statement == mention.statement) {
            candidate.shared_statement = true;
            continue;
        }

        candidate.index = i;
        candidate.seen = true;
        candidate.shared_statement = false;
    }

    std::unordered_set<const AST::ExprNode *> reads;

    for (const auto &[decl, candidate] : last) {
        if (candidate.shared_statement || collector.captured.count(decl) > 0) {
            continue;
        }

        // a declaration this walk never reached is not the body's to hand over - a capture read back
        // off a closure's environment, or a place whose root resolved to somewhere else entirely
        const auto declared = collector.declared.find(decl);

        if (declared == collector.declared.end()) {
            continue;
        }

        const Mention &mention = collector.mentions[candidate.index];

        if (!mention.returning || !(mention.at == declared->second)) {
            continue;
        }

        reads.insert(mention.node);
    }

    return reads;
}
