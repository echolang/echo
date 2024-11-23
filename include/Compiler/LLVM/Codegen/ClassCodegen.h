#ifndef CLASSCODEGEN_H
#define CLASSCODEGEN_H

#pragma once

#include "Compiler/LLVM/Codegen/ClassLayout.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace AST
{
    class ClassAllocExprNode;
    class InstanceOfExprNode;
    class ComplexType;
    class ReleaseNode;
    class RetainExprNode;
    class ValueType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct CmpUnit;

    // everything that is true of a class and of nothing else: allocating the heap block, moving the
    // strong count, and tearing the block down when the count reaches zero
    //
    // it is all here rather than spread over the expression and statement subsystems because the three
    // share one invariant - the block layout in Codegen/ClassLayout.h - and because retain and release
    // have to agree exactly about which word they are moving. the *policy* (which reads retain, which
    // scopes release) is not here at all: AST::OwnershipPass decides that and writes it into the tree,
    // so this file only ever answers "emit one retain here"
    class ClassCodegen
    {
    public:
        ClassCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // a fresh block: malloc, zero it whole, seat the strong count at 1, write the typeinfo
        // pushes the handle
        void gen_class_alloc(AST::ClassAllocExprNode &node);

        // the two nodes AST::OwnershipPass writes into the tree
        void gen_retain_expr(AST::RetainExprNode &node);
        void gen_release_stmt(AST::ReleaseNode &node);

        // `E instanceof T`: the block's identity word against T's identity global. one comparison,
        // because there is no inheritance for a subtype check to walk
        void gen_instanceof(AST::InstanceOfExprNode &node);


        // -= 1, and tear the block down at zero. emits a call to the class's release thunk rather than
        // the sequence itself: a release appears at every scope exit and every overwritten field, and
        // the teardown at zero is not small
        void gen_release(llvm::Value *handle, const AST::ValueType &class_type);

    private:
        CodegenContext &_ctx;

        // += 1 on the strong count of `handle`, which is returned unchanged so this can sit inline in
        // an expression. null-safe, because a class handle is nullable and a retain of null is how
        // `Foo $a = $b;` behaves when `$b` holds nothing.
        //
        // private, unlike gen_release: a retain is only ever a RetainExprNode the ownership pass put
        // in the tree, so nothing outside decides to emit one
        llvm::Value *gen_retain(llvm::Value *handle, const AST::ValueType &class_type);

        // the one release implementation per class per compilation unit, created on first use
        //
        //   void __eco_release_<mangled>(ptr handle)
        //
        // null-check, decrement, return unless zero, call the class's deinit if it has one, free.
        // the deinit is an ordinary Echo function the ownership pass synthesized out of the same
        // emit_drop recursion a struct's scope exit uses - so what a class destroys at zero and what a
        // struct destroys at scope end are decided in exactly one place
        llvm::Function *get_or_create_release_thunk(const AST::ValueType &class_type);

        // libc, declared into the current unit the way printf is. the RC runtime is emitted inline
        // rather than living in the stdlib, so these two are the only external symbols it needs
        llvm::FunctionCallee get_malloc();
        llvm::FunctionCallee get_free();

        // the strong count's address inside `handle`'s block
        llvm::Value *gen_strong_ptr(llvm::Value *handle, const ClassLayout &layout);
    };
};

#endif
