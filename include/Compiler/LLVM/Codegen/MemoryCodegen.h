#ifndef MEMORYCODEGEN_H
#define MEMORYCODEGEN_H

#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the one owner of where heap memory comes from, and of how much of it is outstanding
    //
    // two sites allocated and they shared nothing: ClassCodegen's `malloc` for a class box or a closure
    // environment, and stdlib/core/mem.eco's `extern malloc` for every array, string and raw buffer. So
    // "did this program give everything back" was not merely hard to answer, it was *unanswerable* -
    // no single place saw both halves. Collecting them here is what makes one counter possible, and it
    // is why this is a subsystem rather than two more private methods on ClassCodegen
    //
    // the counting is asked for with --track-allocations rather than implied by a debug build. that is
    // deliberate: a debug build is about the checks a program owes itself, and had this ridden along
    // with it the emitted allocation call would spell itself differently in the two modes - which
    // silently weakens every IR golden written against the wrong one, and leaves a release binary with
    // no way to be leak-checked at all
    class MemoryCodegen
    {
    public:
        MemoryCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // `size` bytes, uninitialized, or null when the allocator could not. With tracking off this is
        // a direct `malloc` call and the seam costs nothing at all - not even the extra call frame the
        // thunk would add
        llvm::Value *gen_alloc(llvm::Value *size, const llvm::Twine &name);

        // resize a block, moving it if it has to. Tracking-wise the interesting one: see the thunk
        llvm::Value *gen_realloc(llvm::Value *block, llvm::Value *size, const llvm::Twine &name);

        // give a block back. null is legal and does nothing, which is C's rule and the one every
        // destructor in the stdlib relies on
        void gen_free(llvm::Value *block);

        // how many allocations are outstanding, as an i64. what `mem::live_allocations()` reads
        //
        // only ever emitted where tracking is on: AST::TypeChecker refuses the builtin without it, so
        // this is not the place that has to have an answer for a counter nobody maintains
        llvm::Value *gen_live_count(const llvm::Twine &name);

        // the `[memory]` section, emitted into `main` just before its return
        //
        // reads --explain-memory itself and emits nothing when it is off, so the entry point's epilogue
        // stays a call rather than a condition. the same shape gen_assert_builtin uses for the build
        // mode, and for the same reason: one reader per option
        void gen_report();

    private:
        CodegenContext &_ctx;

        // the counting allocator, one definition per compilation unit, created on first use
        //
        //   ptr  __eco_alloc(i64 size)
        //   ptr  __eco_realloc(ptr block, i64 size)
        //   void __eco_free(ptr block)
        //
        // `linkonce_odr` for the reason the release thunks and __eco_abort are: every unit that
        // allocates emits its own definition and the linker folds them. Which also means **a body here
        // may not read ambient compiler state** - two copies of one symbol have to be identical, and
        // tests/module_cache.cpp compares two objects byte for byte to hold that
        llvm::Function *get_or_create_alloc_thunk();
        llvm::Function *get_or_create_realloc_thunk();
        llvm::Function *get_or_create_free_thunk();

        // i64 @__eco_alloc_live, zero-initialized
        //
        // `linkonce_odr` rather than one external definition in the entry module, and that is forced
        // rather than chosen: a manifest module's object must not depend on its consumers, and a
        // `declare` here that only `main`'s unit defines is exactly that dependency
        llvm::GlobalVariable *get_or_create_live_counter();

        // non-atomic load / add / store on the counter, the shape ClassCodegen::gen_count_inc uses on a
        // reference count and for the same single-threaded reason. `delta` is signed: a free is -1
        void gen_counter_delta(int64_t delta);

        // the shared skeleton of the three thunks: declare it, name its arguments, and hand back a
        // builder positioned in a fresh entry block. the insert point is restored by the guard the
        // caller holds, because a thunk is created from the middle of whatever body first allocated
        llvm::Function *declare_thunk(
            const std::string &name, llvm::Type *return_type,
            const std::vector<llvm::Type *> &parameter_types,
            const std::vector<const char *> &parameter_names);
    };
};

#endif
