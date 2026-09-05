#ifndef ASTOPERATORREWRITER_H
#define ASTOPERATORREWRITER_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTDetach.h"
#include "AST/ASTNode.h"
#include "AST/ASTOps.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTValueType.h"

#include <vector>

namespace AST
{
    class Bundle;
    class Collector;
    class Module;
    class File;
    class FunctionDeclNode;
    class ExprNode;
    class FunctionCallExprNode;
    class IndexExprNode;
    class ScopeNode;

    // **turns operand syntax into calls, once the types that decide what it means are known.**
    //
    // the parser already asks this question and answers most of it: `$a + $b` over two `Point`s
    // becomes a call to `operator +` where it is written, because the operand types are right there.
    // two shapes it cannot answer, and they share a cause - the type arrives later:
    //
    //  - **a bracket.** `$a[$i]` is a pointer offset or a container's element contract depending on
    //    what `$a` is, and `$a` may be typed from a call the fixpoint has not settled or from a type
    //    parameter nothing has substituted yet
    //  - **an operator over a bare type parameter.** inside `function add<T>(T $a, T $b)` the
    //    operands are `T`, which AST::binary_has_builtin_meaning is deliberately non-committal about,
    //    so the parser takes the built-in path and builds a BinaryExprNode the substituted body then
    //    keeps
    //  - **a write through a bracket.** `$c[$k] = $v` is either one call that owns the whole
    //    insert-or-replace or an ordinary write through a place, and which one it is depends on whether
    //    `$c`'s type declares an element-write contract - AST::declares_index_write
    //
    // so this is **the parser's own decision, re-asked with the types a round produced**. Same
    // predicates, same operand normalizer - AST::parse_time_operand, because this runs before
    // AST::PointerAdjuster and a place read of a borrow is still a pointer here. There is no second
    // rule anywhere in this file, only a second moment.
    //
    // **one walk, five rules**, and the walk is not this file's.
    //
    // AST::RecursiveVisitor owns the descent, so this pass overrides rewrite_value_edge and
    // inherits an enumeration it cannot fall behind. a second complete expression-edge switch
    // would silently miss a new node (`guard`, `??`, `?->`, `strong`). the rules do not differ
    // by edge *position* the way the adjuster's policies do, so both seams answer alike.
    //
    // runs **inside the monomorphizer's fixpoint**, per round, the way AST::OwnershipPass does and
    // for the same reason: it needs the concrete types the round produced, and the call it builds may
    // itself be generic. `operator []` over `array<int32>` is an instantiation, and only the fixpoint
    // can still create one.
    //
    // Ordered ahead of the re-derivation steps, because a declaration inferred from `$a[0]` has no
    // type at all until the element call is attached
    class OperatorRewriter : public RecursiveVisitor
    {
    public:
        explicit OperatorRewriter(Bundle &bundle);

        // one round. answers true when anything was rewritten, which is what keeps the fixpoint going
        bool run_round();

        // **the fixpoint's exit obligation**: one last round in which "not decided yet" is a refusal.
        // being out of rounds is the proof that nothing was ever going to answer, which is
        // Monomorphizer::finalize_calls' reasoning and its moment. after it, no array literal in the
        // tree is still undecided - a permanently undetermined destination is a located diagnostic
        // instead of a literal that silently reaches codegen
        void finalize();

        // the arms whose work is not the base's descent. everything else inherits it
        void visitScope(ScopeNode &node) override;
        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visitVarDecl(VarDeclNode &node) override;
        void visit_assign(AssignNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;
        void visit_index_expr(IndexExprNode &node) override;

        // rule 5's three sites - see upgrade_optional_operand below
        void visit_guard(GuardNode &node) override;
        void visit_null_coalesce(NullCoalesceExprNode &node) override;
        void visit_optional_chain(OptionalChainExprNode &node) override;

    protected:
        // **the rewriting form of one edge**: the expression itself, or its replacement.
        //
        // no positional distinction, so rewrite_place_edge is deliberately *not* overridden - the four
        // rules are about the node, not about where it sits, and a declared operator or a bracket
        // means what it means wherever it is written. the base already routes a place edge here
        ExprNode *rewrite_value_edge(ExprNode *expr) override;

    private:
        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;
        FunctionDeclNode *_current_function = nullptr;

        bool _changed = false;

        // is this the finalizing round? see finalize() above. a flag rather than a parameter because
        // it has to reach expand_array_literal through the visitor's descent, which takes none
        bool _finalizing = false;

        // rule 4 - two mismatched numeric operands are reconciled, for a declaration whose type only a
        // later pass could give it. the *rule* is AST::reconcile_binary_operands', shared with the
        // parser; this is the second moment it has to be applied at
        void widen_binary_operands(BinaryExprNode &bin);

        // rule 1 - a bracket over a container becomes its `operator []`. a base whose type is still
        // undetermined is left alone and asked again next round; everything else is decided once and
        // marked so on the node, which is what stops a reported error being reported every round
        void resolve_index(IndexExprNode &index_expr);

        // language-indexed forms (a pointer GEP and T[N]) have no operator to type the index, so
        // the integer question is asked here. shared so the two cannot drift. answers false when
        // the index is still undetermined (ask again) or was refused
        bool require_integer_index(IndexExprNode &index_expr, const ValueType &base_type);

        // rule 6 - **`$c[...] = v` over a container that declares an element-*write* contract becomes one
        // call that owns the whole write.** `scope.children[index]` is the statement to look at; anything
        // that is not an assignment through a bracket is left alone, and so is one whose receiver declares
        // no write contract - which is `array<T>`, whose brackets are places and whose append form gets
        // its rule from arity.
        //
        // **asked here rather than in visit_assign, and ahead of statement_edge.** resolve_index would
        // otherwise already have decided the bracket is a *read* and moved its operands into that call,
        // which is unrecoverable. the two decisions read one indexed_base_type() at one moment on purpose:
        // split across a round they can disagree, and AST::OwnershipPass walks a body exactly once, ever.
        // an undetermined base is the ordinary "ask again next round" - nothing is marked, so
        // IndexExprNode::resolution_decided keeps OwnershipPass::body_is_concrete answering false.
        //
        // ahead of expand_array_literal too, and that ordering is also load-bearing: after it,
        // `$m[$k] = [1, 2, 3]` would be reported as a literal that names no storage *and then* rewritten
        // anyway, leaving a decided-but-unexpanded literal as a call argument
        void resolve_index_write(ScopeNode &scope, size_t index);

        // rule 3 - a binary or unary node whose operands are concrete now and whose symbol has no
        // built-in meaning for them becomes a call to the declared operator. answers the node itself
        // when nothing changes, and the replacement when it does, so every caller reseats its edge
        ExprNode *resolve_builtin_operator(ExprNode *expr);

        // rule 5 - **the `weak<T>` upgrade the three nullability forms owe, re-asked.** the parser
        // inserts it, through AST::optional_operand_of, which stays the single owner of "a weak is
        // also accepted here"; but inside a template the operand is a bare `T` and there is nothing to
        // upgrade yet, so a `T` that substitutes to a `weak<Node>` reached codegen branching on a
        // handle nobody retained. this is the same function at the moment the substitution
        // happened - exactly the second-moment shape rules 1 and 3 already have.
        //
        // idempotent by construction: what it produces is a nullable, and a nullable is handed back
        // unchanged. answers the operand or its replacement, so every caller reseats its edge
        ExprNode *upgrade_optional_operand(ExprNode *operand, const TokenReference &at);

        // rule 2 - `[1, 2, 3]` becomes a constructor plus one append per element.
        // AST::ArrayLiteralExpansion is the rewrite; this walk places what it returns. a statement
        // whose type is not concrete yet is left for a later round, exactly as an index is
        //
        // the declarations and appends an expression-position hoist produced while walking the
        // current statement. buffered rather than spliced on the spot because the walk is *inside*
        // statement_edge and the scope's child list is what would be mutated under it. saved and
        // restored around a nested scope's own loop, so a literal in an inner block is placed there
        // rather than escaping to the outer one
        std::vector<NodeReference> _hoisted;

        // **the statements rule 6 discarded, batched.** AST::forget_subtree hands its walk to
        // NodeCollection::forget, which is an erase-remove over every bucket of every module - so one call
        // per discarded assignment is one whole-arena sweep per keyed write, per instantiation, per round.
        // AST::ConstFolding batches for exactly this reason, and run_round() is what flushes it
        DetachBatch _detached;

        // how many literals this module has hoisted, so their names are distinct - a statement may
        // hold two (`f([1, 2], [3, 4])`), and a `RAST` golden has to be able to tell them apart
        size_t _hoist_count = 0;

        // **how deep inside a form that does not evaluate its right side we are.** `?->` and `??` are
        // the only two, and a hoist inside either would move the construction above the branch that
        // decides whether it runs. a counter rather than a flag because the two nest
        size_t _hoist_barrier = 0;

        // raises the barrier for one subtree. an RAII guard rather than a set-and-clear, because the
        // arms it sits in return early
        struct HoistBarrier
        {
            explicit HoistBarrier(OperatorRewriter &pass) : _pass(pass) { _pass._hoist_barrier++; }
            ~HoistBarrier() { _pass._hoist_barrier--; }

            HoistBarrier(const HoistBarrier &) = delete;
            HoistBarrier &operator=(const HoistBarrier &) = delete;

        private:
            OperatorRewriter &_pass;
        };

        // the call an operator lowers to. the node is AST::build_operator_call_node's, the same one
        // Parser::build_operator_call builds - this only records that a round changed something.
        // resolution is left to the fixpoint's own settle_calls, where every other pending call is
        // finished
        FunctionCallExprNode &build_operator_call(
            const std::string &spelling,
            OpFixity fixity,
            const TokenReference &at,
            std::vector<ExprNode *> operands
        );

        CodeRef code_ref_for(const TokenReference &token);
    };
};

#endif
