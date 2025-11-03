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
    // the platform hands them to `main` and nowhere else, and Echo has no globals to put them in: a
    // file-scope variable in a library module lowers to a stack slot in whichever function is being
    // emitted, and in a non-entry unit it is dropped entirely. So the entry point records the three
    // words it was given and everything else reads them back from here
    //
    // **the environment arrives as `main`'s third parameter rather than through the `environ` symbol**,
    // and that is what keeps this subsystem free of platform conditionals. `environ` is a *data*
    // symbol, which an `extern` block has no spelling for, and it is not portably addressable anyway -
    // Darwin needs `_NSGetEnviron()`. The three-argument `main` is POSIX on both platforms and a
    // documented CRT extension on Windows, so one seam serves all three
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

        // the three reads, as an i64 count and two opaque pointers. what the `process_argc` /
        // `process_argv` / `process_envp` builtins lower to, in the shape of
        // MemoryCodegen::gen_live_count - a load off a global and nothing else
        llvm::Value *gen_argc(const llvm::Twine &name);
        llvm::Value *gen_argv(const llvm::Twine &name);
        llvm::Value *gen_envp(const llvm::Twine &name);

    private:
        CodegenContext &_ctx;

        // i64 @__eco_argc, ptr @__eco_argv, ptr @__eco_envp - zero-initialized, one definition per
        // compilation unit, created on first use
        //
        // `linkonce_odr` rather than one external definition in the entry module, for the reason
        // MemoryCodegen's counter is: a manifest module's emitted object must not depend on its
        // consumers, and a `declare` here that only `main`'s unit defines is exactly that dependency.
        // The linker folds the copies, so the store in `main` and a load in the stdlib meet on one
        // symbol
        //
        // argc is widened to i64 at the capture rather than stored as the i32 the platform passes,
        // because `usize` is what Echo counts with and one sext in the prologue is cheaper than one
        // at every read
        llvm::GlobalVariable *get_or_create_argc();
        llvm::GlobalVariable *get_or_create_argv();
        llvm::GlobalVariable *get_or_create_envp();
    };
};

#endif
