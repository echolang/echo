#ifndef PROCESSCODEGEN_H
#define PROCESSCODEGEN_H

#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

#include <llvm/ADT/Twine.h>

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the one owner of where a program's arguments and environment come from
    //
    // argc/argv arrive as `main`'s first two parameters on a command. a lib or reactor has
    // no entry, so argc stays 0. envp is the third argument on a native command; on wasi
    // it is `__wasilibc_get_environ()`, on a native lib POSIX `environ` / Darwin
    // `_NSGetEnviron()`, planted from `gen_startup` because there is no `main` to capture
    class ProcessCodegen
    {
    public:
        ProcessCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // the entry point's prologue: store what the platform handed `main` into the three globals
        //
        // emitted unconditionally, into the entry block, before any of the program's own statements -
        // a module-scope `env::arg(1)` is one of them, so a capture that ran later would read a null
        // it had not filled in yet. The whole cost is three stores of registers already in hand
        //
        // takes the function rather than reading `_ctx` for it, because the arguments are the point
        // and a wrong one is worth an assert
        void gen_capture(llvm::Function *entry);

        // a lib or reactor has no `main`. plant `__eco_startup` in `llvm.global_ctors`
        // so wasi-libc's `_initialize` / dlopen fills envp and unbuffers stdio
        void gen_startup();

        // the three reads, as a usize count and two opaque pointers. what the `process_argc` /
        // `process_argv` / `process_envp` builtins lower to, in the shape of
        // MemoryCodegen::gen_live_count - a load off a global and nothing else
        llvm::Value *gen_argc(const llvm::Twine &name);
        llvm::Value *gen_argv(const llvm::Twine &name);
        llvm::Value *gen_envp(const llvm::Twine &name);

    private:
        CodegenContext &_ctx;

        // usize @__eco_argc, ptr @__eco_argv, ptr @__eco_envp - zero-initialized, one definition per
        // compilation unit, created on first use
        //
        // `linkonce_odr` rather than one external definition in the entry module, for the reason
        // MemoryCodegen's counter is: a manifest module's emitted object must not depend on its
        // consumers, and a `declare` here that only `main`'s unit defines is exactly that dependency.
        // The linker folds the copies, so the store in `main` and a load in the stdlib meet on one
        // symbol
        //
        // argc is widened to usize at the capture rather than stored as the i32 the platform passes,
        // because `usize` is what Echo counts with and one sext in the prologue is cheaper than one
        // at every read
        llvm::GlobalVariable *get_or_create_argc();
        llvm::GlobalVariable *get_or_create_argv();
        llvm::GlobalVariable *get_or_create_envp();

        void store_platform_envp();

        // Windows UCRT (stdout and stderr) and wasi-libc (stdout). one owner so
        // gen_capture and gen_startup cannot each remember a different pair
        void unbuffer_stdio();
    };
};

#endif
