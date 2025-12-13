#ifndef ASTACCESSPASS_H
#define ASTACCESSPASS_H

#pragma once

#include "AST/ASTAccess.h"
#include "AST/ASTRecursiveVisitor.h"

namespace AST
{
    class Bundle;
    class Collector;
    class Module;
    class File;

    // **enforces the exclusivity half of the access rule: addresses may alias, accesses may not
    // conflict.**
    //
    // an argument list is where two accesses to one region are visible at once and where the caller
    // is still the one who can do something about it, so that is where this asks. for each call it
    // builds an AST::AccessPath per argument and an AST::AccessEffect per parameter, and refuses the
    // pairs that both overlap and conflict.
    //
    // **only `t_overlap` is refused, never `t_unknown`.** the rule may cost a program that was legal
    // nothing it did not ask for, so a path this cannot see through is left alone - which is also why
    // the pass is honest about what it does not license: it proves nothing about what a *callee*
    // reaches, and a callee can reach the same storage through a class handle it holds or a pointer
    // it stored. two arguments being disjoint here is not a `noalias`, and nothing downstream may
    // read it as one
    //
    // runs in run_semantic_passes **after AST::PointerAdjuster**, for two reasons that are really one:
    // by then every call carries a `decl` and every deref is a node, so a path walk sees the tree the
    // program actually means rather than the one it was written as. deliberately *not* inside the
    // monomorphizer's fixpoint - it mints no calls and needs no round, so it is also free of the
    // walk-a-body-once constraint AST::OwnershipPass lives under
    class AccessPass : public RecursiveVisitor
    {
    public:
        AccessPass(Bundle &bundle);

        void run();

        void visitFunctionCallExpr(FunctionCallExprNode &node) override;

        // the *body* half of the rule. a `read` parameter promises the region it names is only read,
        // and until something checks that, the promise is a comment: `const` is a per-level flag, so
        // it does not reach the pointee of a member pointer, and
        // `$src->storage->data:$[0] = 999;` through a `const array<int32>&` writes the caller's array.
        //
        // **this is what an emitted `readonly` would rest on**, which is why it is here rather than
        // deferred: an LLVM `readonly` says the function writes nothing through that argument or
        // anything based on it, and without this check that claim is simply false
        void visit_assign(AssignNode &node) override;
        void visitFunctionDecl(FunctionDeclNode &node) override;

    private:
        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        // whose parameters a write is measured against. null at file scope, where a write reaches no
        // parameter at all
        FunctionDeclNode *_current_function = nullptr;

        // the `read` parameter of *this* function whose region an expression reaches, or null - which
        // is every ordinary expression. one walk, shared by the two halves of the promise: the write
        // that goes through it, and the call that hands it somewhere it could be written
        //
        // **declared `read` only, never one inferred from `const`.** `const` is a per-level flag and
        // has always permitted a write through a member pointer, so widening this would refuse
        // programs that were legal before - which is the one thing the rule may not do
        const VarDeclNode *read_parameter_reached_by(ExprNode *expr) const;

        // does this argument hand a `read` region somewhere that could write it?
        void check_read_escape(FunctionCallExprNode &node, size_t index);

        // the pairwise check over one settled call. separate from the visit so the descent and the
        // rule are not one function, and so a second asker - an indirect call, once a callable value
        // can carry effects - has something to call
        void check_call(FunctionCallExprNode &node);

        // the sentence, the second location and the remedy for one refused pair. built here rather
        // than in the issue so that the issue stays a value with no knowledge of declarations
        void report_conflict(
            FunctionCallExprNode &node,
            size_t first,
            size_t second,
            AccessEffect first_effect,
            AccessEffect second_effect);
    };
};

#endif
