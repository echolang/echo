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
    class ExprNode;
    class FunctionCallExprNode;
    class IndexExprNode;
    class ArrayLiteralExprNode;
    class ScopeNode;
    class VarDeclNode;
    class ComplexType;
    class Namespace;

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
    // It used to be a second complete expression-edge switch parallel to PointerAdjuster's, which is
    // exactly where a forgotten arm is a silent miss. Both of them had forgotten the same four
    // (`guard`, `??`, `?->`, `strong`). AST::RecursiveVisitor owns the descent now, so this pass
    // overrides rewrite_value_edge and inherits an enumeration it cannot fall behind. The rules do
    // not differ by edge *position* the way the adjuster's policies do, so both seams answer alike.
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

        bool _changed = false;

        // is this the finalizing round? see finalize() above. a flag rather than a parameter because
        // it has to reach expand_array_literal through the visitor's descent, which takes none
        bool _finalizing = false;

        // rule 4 - two mismatched numeric operands are reconciled, for a declaration whose type only a
        // later pass could give it. the *rule* is AST::common_numeric_type's, shared with the parser;
        // this is the second moment it has to be applied at, and only the cast is built here
        void widen_binary_operands(BinaryExprNode &bin);

        // rule 1 - a bracket over a container becomes its `operator []`. a base whose type is still
        // undetermined is left alone and asked again next round; everything else is decided once and
        // marked so on the node, which is what stops a reported error being reported every round
        void resolve_index(IndexExprNode &index_expr);

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

        // rule 2 - `[1, 2, 3]` becomes a constructor of the destination type plus one append per
        // element, spliced into `scope` after `index`. only the enclosing scope can do this: the
        // expansion is several statements, and an expression has nowhere to put them
        //
        // `scope.children[index]` is the statement to look at. a destination whose type is not
        // concrete yet is left for a later round, exactly as an index is
        void expand_array_literal(ScopeNode &scope, size_t index);

        // **rule 2 where the author named no storage** - `f([1, 2, 3])`. the compiler names it: a
        // synthesized declaration is hoisted *ahead* of the statement and the literal's edge becomes
        // that declaration's name, so what reaches the parameter is an ordinary place taking an
        // ordinary `t_borrow`. none of AST::OwnershipPass's temporary machinery is involved, which is
        // the point - a literal is `t_addressless` precisely because it fills storage rather than
        // occupying some
        //
        // answers the replacement for the edge, or null when the literal is not ready - it has no
        // AST::bind_array_literal_to type yet, so the round that settles the call has not happened.
        // the caller leaves the literal in place, and finalize() is what turns a permanent "not yet"
        // into the diagnostic
        ExprNode *hoist_array_literal(ArrayLiteralExprNode &literal);

        // the constructor of `type` written into `slot`, plus one `$into[] = element` per element.
        // shared by the two rules above because it is the whole of what an expansion *is*; what
        // differs between them is only where the storage came from and where the appends go
        //
        // answers false when the destination cannot be built from a literal, having reported it
        bool build_literal_expansion(
            ArrayLiteralExprNode &literal,
            VarDeclNode &into,
            const ValueType &type,
            ExprNode **slot,
            std::vector<NodeReference> &appends);

        // the declarations and appends hoist_array_literal produced while walking the current
        // statement, innermost first. buffered rather than spliced on the spot because the walk is
        // *inside* statement_edge and the scope's child list is what would be mutated under it
        //
        // saved and restored around a nested scope's own loop, so a literal in an inner block is
        // wrapped there rather than escaping to the outer one
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

        // moves `scope.children[index]` into a scope of its own, preceded by whatever _hoisted holds.
        // that scope *is* the lifetime: the declarations are its locals, so the statement's end is
        // where AST::OwnershipPass destroys them
        void wrap_statement_with_hoists(ScopeNode &scope, size_t index);

        // the two statement shapes an array literal may sit in, resolved to the one thing the
        // expansion needs: the declaration whose storage is being filled, and the literal filling it.
        // `decl` null means this statement is not one of them
        struct LiteralDestination
        {
            VarDeclNode *decl = nullptr;
            ArrayLiteralExprNode *literal = nullptr;
            ExprNode **slot = nullptr;

            // is this statement the declaration itself? **only a declaration may be typed *from* its
            // elements** - an assignment writes storage somebody else already named, and taking a
            // type off the right-hand side there would let `$a = ["x"];` silently retype an
            // `array<int32>` rather than being the mismatch it is
            bool declares = false;
        };

        static LiteralDestination literal_destination(Node *statement);

        // **what type is being filled** - the declaration's own, or the one its elements give it when
        // it was written without one. answers false when the expansion must not proceed: refused,
        // or not decided yet, which are the two states that own the literal's diagnostic.
        //
        // its own function because it is the whole of "which type", and expand_array_literal above is
        // the whole of "emit the constructor and the appends" - one scope holding both was the two
        // sets of locals in each other's way
        bool settle_destination_type(const LiteralDestination &destination, ValueType &settled);

        // **an array literal no statement claimed.** the two positions that can hold one are the
        // vardecl and assign arms above; every other position reports, and reports the same thing -
        // one function, because the two askers are the statement walk and the expression walk and a
        // string each is a string that drifts. quiet once decided, so a round does not report twice
        void report_unplaced_literal(ArrayLiteralExprNode &literal);

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

        // a pending call that is *not* an operator's - the destination type's constructor, which the
        // array literal expansion names. its own function rather than a flag on the one above,
        // because the lookup point is the whole difference: an operator resolves in the root
        // namespace and a type's constructor resolves where the type was declared
        FunctionCallExprNode &build_call(
            const std::string &name,
            const TokenReference &at,
            std::vector<ExprNode *> operands,
            const Namespace *lookup
        );

        CodeRef code_ref_for(const TokenReference &token);
    };
};

#endif
