#ifndef ASTCONSTFOLDING_H
#define ASTCONSTFOLDING_H

#pragma once

#include "AST/ASTRecursiveVisitor.h"

#include "Token.h"

#include <optional>
#include <string>
#include <unordered_set>

namespace AST
{
    class Bundle;
    class Collector;
    struct CodeRef;
    class ConstExprNode;
    class ConstIfNode;
    class ExprNode;
    class File;
    class Module;
    class ScopeNode;
    struct ConstFoldResult;

    // decides everything the language asked the compiler to decide, and removes the marker afterwards.
    //
    //     const if (mem::is_trivially_copyable<T>()) { A }   ->   { A }
    //     else                                       { B }
    //
    //     const(4 * 8)                                       ->   literal<int32>(32)
    //
    // two rules and one pass, because they share the round slot, the finalizing flag and the folder - and
    // because a `const(...)` inside a `const if`'s condition has to settle in the same walk. the statement
    // rule lives in visitScope and the expression rule in rewrite_value_edge, which is the split
    // AST::OperatorRewriter already has for the same reason.
    //
    // **inside AST::Monomorphizer's fixpoint, immediately after instantiate_generic_calls and ahead of
    // everything else in the round.** three constraints pin that position and all three are load-bearing;
    // the call site states them, because that is where a reader trying to move it will be looking.
    //
    // the tree is walked through scope children and **never** through nodes.of_type<ConstIfNode>():
    // NodeCollection owns a detached node forever, so an of_type sweep would re-process one that had
    // already been folded away - and would blame arms that were correctly discarded.
    //
    // **built on AST::RecursiveVisitor**, AST::ForeachLowering's reason verbatim: a hand-rolled statement
    // walk is a second answer to "what are this node's owned children", and it fails by omission - a
    // missing arm silently never reaches the subtree under it
    class ConstFolding : private RecursiveVisitor
    {
    public:
        ConstFolding(Bundle &bundle);

        // answers whether anything changed, so the fixpoint can report progress
        bool run_round();

        // **the fixpoint's exit obligation**: one last round in which "not answerable yet" is a refusal.
        // being out of rounds is the proof that nothing was ever going to answer it, which is
        // Monomorphizer::finalize_calls' reasoning and its moment.
        //
        // without it `t_pending` has no exit at all: a condition naming a call that never settles would
        // leave the node in the tree, and PointerAdjuster's throw for a survivor aborts the process on top
        // of a perfectly good diagnostic nobody had printed yet
        void finalize();

    private:
        CodeRef code_ref_for(const TokenReference &token);

        // indexed rather than the base's ranged walk, because lower() reseats the child in place and the
        // arm it seated is descended into on the same index - which is what lets a `const if` nested in an
        // arm, and an `else const if` chain, settle in this same round
        void visitScope(ScopeNode &node) override;

        // a template's body is only meaningful once cloned into a concrete instance, and the type its
        // condition asks about is exactly what is not known there. **this is the arm that keeps
        // `array<T>` compilable at all**: its own `const if (mem::is_trivially_copyable<T>())` can never
        // fold against an unbound `T`, so without the skip finalize() would refuse the template rather
        // than each instance deciding for itself. AST::ForeachLowering's rule, and PointerAdjuster's
        // before it
        void visitFunctionDecl(FunctionDeclNode &node) override;

        // the expression rule. descends first, then folds whatever came back if it is a `const(...)` -
        // so a nested one settles innermost first and the outer sees a literal
        ExprNode *rewrite_value_edge(ExprNode *expr) override;

        // lowers `scope.children[index]`, which is a ConstIfNode: splices in the arm its condition
        // selected, leaves it in place while the condition is not answerable yet, and discards it after
        // a refusal - because a refused node must not survive. run_semantic_passes runs PointerAdjuster
        // *before* it gates on has_critical_issues(), so a survivor turns a good located diagnostic into
        // an unlocated internal error stacked on top of it
        void lower(ScopeNode &scope, size_t index);

        // reports and discards, in that order - one function, because forgetting the discard is the
        // silent half and there are three arms that must not forget it
        void refuse(ScopeNode &scope, size_t index, ConstIfNode &branch, std::string why);

        // **neither arm survives a refusal.** keeping one would be compiling half a program on a guess,
        // and would bury the one real diagnostic under whatever that arm had to say
        void discard(ScopeNode &scope, size_t index, ConstIfNode &branch);

        // seats `arm` where the branch was, and detaches the branch from everything it held
        void splice(ScopeNode &scope, size_t index, ConstIfNode &branch, ScopeNode &arm);

        // the expression rule's refuse+discard, the statement rule's refuse() shape. `why` is empty for
        // the one case that discards without a message of its own - a pending marker where something
        // else already explained why nothing settled
        ExprNode *refuse_expr(ConstExprNode &marker, std::optional<std::string> why);

        // the marker leaves the tree, whatever it is being replaced by - see the implementation
        void detach(ConstExprNode &marker);

        // **batched, and run_round() is what flushes it.** AST::forget_subtree is a sweep over every
        // arena in the bundle, so paying it per discarded subtree inside the fixpoint is the cost this
        // pass would otherwise add to every round
        void forget(Node &root);

        // the literal a folded result becomes, minted into the current module's arena
        ExprNode *literal_for(const ConstFoldResult &folded, const TokenReference &at);


        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        bool _changed = false;

        // is this the finalizing round? see finalize(). a flag rather than a parameter because it has to
        // reach lower() through the visitor's descent, which takes none
        bool _finalizing = false;

        // what left the tree this round, handed to the arena in one go at the end of it
        std::unordered_set<const Node *> _detached;
    };
};

#endif
