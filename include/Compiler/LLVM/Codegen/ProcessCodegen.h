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
    // it is `__wasilibc_get_environ()`. a lib reads the platform source on every
    // `process_envp` so a host `setenv` after `dlopen` is visible
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

        void gen_startup();

        // `fini_walker` is null when nothing is owed a teardown
        void gen_library_lifecycle(llvm::Function *fini_walker);

        // the three reads, as a usize count and two opaque pointers. what the `process_argc` /
        // `process_argv` / `process_envp` builtins lower to. argc/argv load the capture
        // globals. envp does too on a command; a lib reads the platform source live
        llvm::Value *gen_argc(const llvm::Twine &name);
        llvm::Value *gen_argv(const llvm::Twine &name);
        llvm::Value *gen_envp(const llvm::Twine &name);

        // the per-process hash seed `hash::of` folds in. **a plain load**, not a lazy static: that
        // acquire would sit in every map probe. written once before anything hashes; constant 0 under `--fixed-hash-seed`.
        // a native lib copies the host's exported cell at load so host and plugin hash the same key
        llvm::Value *gen_hash_seed(const llvm::Twine &name);

        // **the one owner of what a failed write to stdout or stderr does.** a negative `result` on a
        // row that ignores SIGPIPE branches to `__eco_stdio_write_failed`. emits nothing on any other row
        void gen_stdout_write_check(llvm::Value *result);

        // the `stdio_write_failed` builtin: the same helper, after a write the stdlib already knows failed
        void gen_stdio_write_failed(llvm::Value *fd);

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

        llvm::Value *load_platform_envp();
        void store_platform_envp();

        // uint64 @__eco_hash_seed, linkonce_odr like the three above, DLLExport so a lib does not hide it
        llvm::GlobalVariable *get_or_create_hash_seed();

        // fills it: `ECHO_HASH_SEED` when set, else CodegenTarget::entropy_source(). the cell is still
        // emitted when the seed is fixed, so a plugin can copy a zero
        void gen_seed_hash();

        // the two arms of unbuffer_stdio, one per StdioUnbuffering that does something
        void unbuffer_ucrt(llvm::Type *i32, llvm::Type *ptr);
        void unbuffer_wasi_libc(llvm::Type *i32, llvm::Type *ptr);

        // Windows UCRT (stdout and stderr) and wasi-libc (stdout). one owner so
        // gen_capture and gen_startup cannot each remember a different pair
        void unbuffer_stdio();

        void binary_stdio();
        void ignore_sigpipe();
        bool ignores_sigpipe() const;
        llvm::Function *get_or_create_stdio_write_failed();
        llvm::Value *gen_errno_location();
    };
};

#endif
