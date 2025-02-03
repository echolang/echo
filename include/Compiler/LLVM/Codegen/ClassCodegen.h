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


        // "move the strong count of this value", whatever kind of value it is. the one entry point for
        // both, so a teardown site added later cannot forget that a callable counts its environment
        // rather than a class block - which would be the wrong thunk with no diagnostic
        //
        // the retain hands the value back unchanged, so it can sit inline in an expression
        llvm::Value *gen_retain_value(llvm::Value *value, const AST::ValueType &type);
        void gen_release_value(llvm::Value *value, const AST::ValueType &type);

        // the heap block for a class value: malloc, zero, strong count 1, typeinfo. shared by
        // gen_class_alloc and by a closure's environment, which is a class the compiler declared rather
        // than one the user did - so the two cannot end up with differently shaped blocks
        llvm::Value *gen_class_box_alloc(const AST::ValueType &class_type);

        // the strong count of `handle` as an i64, or **0 when it is null**. the third reference-count
        // operation, here rather than at its caller so `ClassBox::strong_index` keeps one owner.
        //
        // reached from Echo through the `ref_count` builtin, whose one real consumer is a copy-on-write
        // container asking "am I the only owner". see the implementation for why null answers zero
        llvm::Value *gen_strong_count(llvm::Value *handle, const AST::ValueType &class_type);

    private:
        CodegenContext &_ctx;

        // the two arms gen_retain_value / gen_release_value dispatch to. a class moves the count in its
        // block; a callable moves the one in its *environment*, which is the only thing a callable owns
        //
        // the callable arm is uniform rather than per type, and that is forced: a callable's static type
        // is its signature and says nothing about which environment it holds, so the teardown cannot be
        // keyed on a class the way the class arm is. it stays correct because an environment holds no
        // owning capture - one that would is rejected at the capture site (todo/A27)
        llvm::Value *gen_retain(llvm::Value *handle, const AST::ValueType &class_type);
        void gen_release(llvm::Value *handle, const AST::ValueType &class_type);

        llvm::Value *gen_callable_retain(llvm::Value *callable);
        void gen_callable_release(llvm::Value *callable);

        // += 1 on the strong count of `block`, guarded on `block` being non-null - a class handle is
        // nullable and a retain of null is how `Foo $a = $b;` behaves when `$b` holds nothing.
        //
        // a null `layout` means the count is the block's first word, which is how an environment is
        // reached: the one thing a callable does not know is a class layout
        void gen_strong_inc(llvm::Value *block, const ClassLayout *layout, const char *label);

        // the one release implementation per class per compilation unit, created on first use
        //
        //   void __eco_release_<mangled>(ptr handle)
        //
        // null-check, decrement, return unless zero, call the class's deinit if it has one, free.
        // the deinit is an ordinary Echo function the ownership pass synthesized out of the same
        // emit_drop recursion a struct's scope exit uses - so what a class destroys at zero and what a
        // struct destroys at scope end are decided in exactly one place
        llvm::Function *get_or_create_release_thunk(const AST::ValueType &class_type);

        // the environment counterpart, one per compilation unit rather than one per type:
        //
        //   void __eco_release_env(ptr handle)
        //
        // no deinit, because an environment holds no owning capture - see gen_callable_release
        llvm::Function *get_or_create_env_release_thunk();

        // the body both of the above are: linkonce_odr `void <name>(ptr handle)`, null-check, decrement,
        // return unless zero, `complex`'s deinit if there is one, free. built with its own builder
        // position, which is saved and restored - this is called from the middle of whatever function
        // asked for a release
        //
        // `layout` and `complex` are both null for an environment, which has neither
        llvm::Function *build_release_thunk(
            const std::string &name, const ClassLayout *layout, const AST::ComplexType *complex);

        // libc, declared into the current unit the way printf is. the RC runtime is emitted inline
        // rather than living in the stdlib, so these two are the only external symbols it needs
        llvm::FunctionCallee get_malloc();
        llvm::FunctionCallee get_free();

        // the strong count's address inside `handle`'s block
        llvm::Value *gen_strong_ptr(llvm::Value *handle, const ClassLayout &layout);
    };
};

#endif
