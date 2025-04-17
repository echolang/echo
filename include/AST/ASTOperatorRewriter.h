#ifndef ASTOPERATORREWRITER_H
#define ASTOPERATORREWRITER_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTNode.h"
#include "AST/ASTOps.h"

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
    //    keeps (todo/A32)
    //
    // so this is **the parser's own decision, re-asked with the types a round produced**, through the
    // very same predicates and the very same operand normalizer - AST::parse_time_operand, because
    // this runs before AST::PointerAdjuster and a place read of a borrow is still a pointer here.
    // there is no second rule anywhere in this file, only a second moment.
    //
    // it is a *rewriter*, not a walker, for AST::PointerAdjuster's reason: it replaces the parent's
    // edge to a child, so it drives its own traversal rather than subclassing RecursiveVisitor.
    //
    // **one walk, three rules**, weighed rather than assumed (todo/A32 asks for exactly this trade):
    // a second complete expression-edge switch parallel to PointerAdjuster::adjust is a place where a
    // forgotten arm is a silent miss, and three switches would be three of them. the rules do not
    // differ by edge *position* the way the adjuster's policies do, so one switch serves all three.
    //
    // runs **inside the monomorphizer's fixpoint**, per round, the way AST::OwnershipPass does and
    // for the same reason: it needs the concrete types the round produced, and the call it builds may
    // itself be generic - `operator []` over `Array<int32>` is an instantiation, which only the
    // fixpoint can still create. it is ordered ahead of the re-derivation steps because a declaration
    // inferred from `$a[0]` has no type at all until the element call is attached
    class OperatorRewriter
    {
    public:
        explicit OperatorRewriter(Bundle &bundle);

        // one round. answers true when anything was rewritten, which is what keeps the fixpoint going
        bool run_round();

    private:
        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        bool _changed = false;

        // walks the node, rewriting each expression edge it owns
        void rewrite(Node *node);

        // the rewriting form of one edge: the expression itself, or its replacement
        ExprNode *rewrite_expr(ExprNode *expr);

        // rule 1 - a bracket over a container becomes its `operator []`. a base whose type is still
        // undetermined is left alone and asked again next round; everything else is decided once and
        // marked so on the node, which is what stops a reported error being reported every round
        void resolve_index(IndexExprNode &index_expr);

        // rule 3 - a binary or unary node whose operands are concrete now and whose symbol has no
        // built-in meaning for them becomes a call to the declared operator. answers the node itself
        // when nothing changes, and the replacement when it does, so every caller reseats its edge
        ExprNode *resolve_builtin_operator(ExprNode *expr);

        // rule 2 - `[1, 2, 3]` becomes a constructor of the destination type plus one append per
        // element, spliced into `scope` after `index`. only the enclosing scope can do this: the
        // expansion is several statements, and an expression has nowhere to put them
        //
        // `scope.children[index]` is the statement to look at. a destination whose type is not
        // concrete yet is left for a later round, exactly as an index is
        void expand_array_literal(ScopeNode &scope, size_t index);

        // the two statement shapes an array literal may sit in, resolved to the one thing the
        // expansion needs: the declaration whose storage is being filled, and the literal filling it.
        // `decl` null means this statement is not one of them
        struct LiteralDestination
        {
            VarDeclNode *decl = nullptr;
            ArrayLiteralExprNode *literal = nullptr;
            ExprNode **slot = nullptr;
        };

        static LiteralDestination literal_destination(Node *statement);

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
            std::vector<ExprNode *> operands);

        // a pending call that is *not* an operator's - the destination type's constructor, which the
        // array literal expansion names. its own function rather than a flag on the one above,
        // because the lookup point is the whole difference: an operator resolves in the root
        // namespace and a type's constructor resolves where the type was declared
        FunctionCallExprNode &build_call(
            const std::string &name,
            const TokenReference &at,
            std::vector<ExprNode *> operands,
            const Namespace *lookup);

        CodeRef code_ref_for(const TokenReference &token);
    };
};

#endif
