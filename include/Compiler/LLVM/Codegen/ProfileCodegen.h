#ifndef PROFILECODEGEN_H
#define PROFILECODEGEN_H

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <cstdint>
#include <string>

namespace AST
{
    class ExprNode;
    class FunctionDeclNode;
    class ValueType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the one owner of which seam reports which event. every method begins
    // `if (!_ctx.options.profiling()) return;`, so a build without the flag
    // emits the bytes it emits today
    class ProfileCodegen
    {
    public:
        ProfileCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // `"frame-pointer"="all"` on every defined function, plus the module's
        // FramePointerKind::All for functions LLVM creates itself. after the drain,
        // before finalize_all
        void mark_frame_pointers();

        void emit_start();
        void emit_finish(int status);

        // every ReturnInst of `main`, including a file-scope return that skipped the epilogue
        void instrument_main_returns(llvm::Function *main);

        void emit_alloc(llvm::Value *block, llvm::Value *size);
        void emit_realloc(llvm::Value *old_block, llvm::Value *block, llvm::Value *size);
        void emit_free(llvm::Value *block);

        void instrument_copy_function(llvm::Function &fn, const AST::FunctionDeclNode &decl);
        void note_arrival(AST::ExprNode &node);

        void instrument_body(llvm::Function &fn, const AST::FunctionDeclNode &decl);

    private:
        CodegenContext &_ctx;

        void attach_location();
        llvm::FunctionCallee hook(
            const char *name,
            llvm::Type *return_type,
            llvm::ArrayRef<llvm::Type *> parameter_types
        );
        llvm::GlobalVariable *site_constant(
            uint32_t kind,
            uint64_t type_bytes,
            const std::string &type_name,
            llvm::Function *function
        );
        llvm::StructType *site_type();
        llvm::Value *as_i64(llvm::Value *value);
        void emit_copy(uint32_t kind, const AST::ValueType &type, llvm::Function *function);
    };
};

#endif
