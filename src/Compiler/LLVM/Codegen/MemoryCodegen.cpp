#include "Compiler/LLVM/Codegen/MemoryCodegen.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/Codegen/ProfileCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <cassert>

namespace Compiler::LLVM
{

namespace
{
    // the emitted runtime's symbols, spelled once. an IR check names them, so they are part of the
    // compiler's observable surface rather than an implementation detail
    constexpr const char *k_alloc_symbol = "__eco_alloc";
    constexpr const char *k_realloc_symbol = "__eco_realloc";
    constexpr const char *k_free_symbol = "__eco_free";
    constexpr const char *k_live_counter_symbol = "__eco_alloc_live";
    constexpr const char *k_cache_alloc_symbol = "__eco_cache_alloc";
    constexpr const char *k_cache_realloc_symbol = "__eco_cache_realloc";
    constexpr const char *k_cache_free_symbol = "__eco_cache_free";
    constexpr const char *k_cache_family = "__eco_cache";
};

llvm::Function *MemoryCodegen::declare_thunk(
    const std::string &name,
    llvm::Type *return_type,
    const std::vector<llvm::Type *> &parameter_types,
    const std::vector<const char *> &parameter_names
)
{
    assert(parameter_types.size() == parameter_names.size()
        && "a thunk parameter without a name would leave the IR unreadable");

    llvm::Function *thunk = llvm::Function::Create(
        llvm::FunctionType::get(return_type, parameter_types, false),
        llvm::GlobalValue::LinkOnceODRLinkage,
        name,
        _ctx.current_module());

    for (size_t index = 0; index < parameter_names.size(); index++) {
        thunk->getArg(static_cast<unsigned>(index))->setName(parameter_names[index]);
    }

    _ctx.set_insert_point(llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", thunk));

    return thunk;
}

// `noalias` on the return: the block does not overlap anything the caller holds. the JIT runs
// no FunctionAttrs pass, so this has to be stated. no `allockind` / alloc-family: this thunk
// exists only under `--track-allocations`, and those attributes would let a pass delete a
// block whose only observable effect is the counter beside it
void MemoryCodegen::mark_allocating_thunk(llvm::Function *thunk, unsigned size_argument)
{
    thunk->addRetAttr(llvm::Attribute::NoAlias);
    thunk->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
        thunk->getContext(), size_argument, std::nullopt));
}

llvm::FunctionCallee MemoryCodegen::cache_callee(
    const char *name,
    llvm::Type *return_type,
    const std::vector<llvm::Type *> &parameter_types,
    MemoryCodegen::CacheCallee kind
)
{
    llvm::FunctionCallee callee = _ctx.current_module()->getOrInsertFunction(
        name,
        llvm::FunctionType::get(return_type, parameter_types, false));

    auto *fn = llvm::dyn_cast<llvm::Function>(callee.getCallee());
    if (fn == nullptr || fn->hasFnAttribute("alloc-family")) {
        return callee;
    }

    // the three declarations are one family, and free carries allocptr. marking
    // only the alloc is what used to lower a dead block to an alloca and leave
    // __eco_cache_free on it. the counting thunk stays unmarked: its counter is
    // the observable effect isRemovableAlloc would drop
    llvm::AllocFnKind alloc_kind;
    switch (kind) {
    case CacheCallee::t_free:
        alloc_kind = llvm::AllocFnKind::Free;
        fn->addParamAttr(0, llvm::Attribute::AllocatedPointer);
        break;
    case CacheCallee::t_alloc:
        alloc_kind = llvm::AllocFnKind::Alloc | llvm::AllocFnKind::Uninitialized;
        fn->addRetAttr(llvm::Attribute::NoAlias);
        fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
            fn->getContext(), 0, std::nullopt));
        break;
    case CacheCallee::t_realloc:
        alloc_kind = llvm::AllocFnKind::Realloc;
        fn->addRetAttr(llvm::Attribute::NoAlias);
        fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
            fn->getContext(), 1, std::nullopt));
        fn->addParamAttr(0, llvm::Attribute::AllocatedPointer);
        break;
    }

    fn->addFnAttr(llvm::Attribute::get(
        fn->getContext(),
        llvm::Attribute::AllocKind,
        static_cast<uint64_t>(alloc_kind)));
    fn->addFnAttr("alloc-family", k_cache_family);

    return callee;
}

llvm::FunctionCallee MemoryCodegen::heap_callee(CacheCallee kind)
{
    llvm::Type *size_ty = _ctx.size_int_ty();
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();
    llvm::Type *void_ty = llvm::Type::getVoidTy(*_ctx.llvm_context);
    const bool page_heap = _ctx.options.using_page_heap();

    switch (kind) {
    case CacheCallee::t_alloc:
        if (!page_heap) {
            return _ctx.libc_callee(Compiler::RuntimeSymbol::t_malloc, opaque_ptr, { size_ty });
        }
        return cache_callee(k_cache_alloc_symbol, opaque_ptr, { size_ty }, kind);
    case CacheCallee::t_realloc:
        if (!page_heap) {
            return _ctx.libc_callee(
                Compiler::RuntimeSymbol::t_realloc, opaque_ptr, { opaque_ptr, size_ty });
        }
        return cache_callee(
            k_cache_realloc_symbol, opaque_ptr, { opaque_ptr, size_ty }, kind);
    case CacheCallee::t_free:
        if (!page_heap) {
            return _ctx.libc_callee(Compiler::RuntimeSymbol::t_free, void_ty, { opaque_ptr });
        }
        return cache_callee(k_cache_free_symbol, void_ty, { opaque_ptr }, kind);
    }

    assert(false && "CacheCallee has no arm");
    return {};
}

llvm::FunctionCallee MemoryCodegen::heap_alloc()
{
    return heap_callee(CacheCallee::t_alloc);
}

llvm::FunctionCallee MemoryCodegen::heap_realloc()
{
    return heap_callee(CacheCallee::t_realloc);
}

llvm::FunctionCallee MemoryCodegen::heap_free()
{
    return heap_callee(CacheCallee::t_free);
}

llvm::GlobalVariable *MemoryCodegen::get_or_create_live_counter()
{
    return _ctx.get_or_create_odr_global(
        k_live_counter_symbol, llvm::Type::getInt64Ty(*_ctx.llvm_context));
}

void MemoryCodegen::gen_counter_delta(int64_t delta)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::GlobalVariable *counter = get_or_create_live_counter();
    llvm::Constant *step = llvm::ConstantInt::get(i64, delta);

    // one global counter, existing only under --track-allocations. monotonic: it answers how
    // many, at some moment, and licenses nothing. a row with OS threads is an RMW; a row
    // without is a load and a store
    if (_ctx.options.codegen.has_os_threads()) {
        _ctx.builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::Add,
            counter,
            step,
            _ctx.layout().getABITypeAlign(i64),
            llvm::AtomicOrdering::Monotonic)->setName("live");
        return;
    }

    llvm::Value *current = _ctx.builder->CreateLoad(i64, counter, "live");
    _ctx.builder->CreateStore(
        _ctx.builder->CreateAdd(current, step, "live.next"),
        counter);
}

llvm::Function *MemoryCodegen::get_or_create_alloc_thunk()
{
    if (llvm::Function *existing = _ctx.current_module()->getFunction(k_alloc_symbol)) {
        return existing;
    }

    llvm::Type *size_ty = _ctx.size_int_ty();
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);

    llvm::Function *thunk = declare_thunk(k_alloc_symbol, opaque_ptr, { size_ty }, { "size" });

    // the byte count is the only argument
    mark_allocating_thunk(thunk, 0);

    llvm::Value *block = _ctx.builder->CreateCall(heap_alloc(), { thunk->getArg(0) }, "block");

    // **a failed allocation is not an allocation.** `mem::alloc` is documented to hand back null when
    // the allocator could not, so counting the attempt would leave a program that survived an OOM
    // reporting a leak it does not have
    llvm::BasicBlock *count_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "alloc.count", thunk);
    llvm::BasicBlock *done_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "alloc.done", thunk);

    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(block), done_block, count_block);

    _ctx.set_insert_point(count_block);
    gen_counter_delta(1);
    _ctx.profile->emit_alloc(block, thunk->getArg(0));
    _ctx.builder->CreateBr(done_block);

    _ctx.set_insert_point(done_block);
    _ctx.builder->CreateRet(block);

    return thunk;
}

llvm::Function *MemoryCodegen::get_or_create_free_thunk()
{
    if (llvm::Function *existing = _ctx.current_module()->getFunction(k_free_symbol)) {
        return existing;
    }

    llvm::Type *void_type = llvm::Type::getVoidTy(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);

    llvm::Function *thunk = declare_thunk(k_free_symbol, void_type, { opaque_ptr }, { "block" });

    llvm::Value *block = thunk->getArg(0);

    // `free(null)` is legal C and does nothing, so it moves the count by nothing. a destructor over a
    // field that was never allocated takes this path, which makes it ordinary rather than an edge case
    llvm::BasicBlock *count_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "free.count", thunk);
    llvm::BasicBlock *done_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "free.done", thunk);

    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(block), done_block, count_block);

    _ctx.set_insert_point(count_block);
    gen_counter_delta(-1);
    _ctx.profile->emit_free(block);
    _ctx.builder->CreateCall(heap_free(), { block });
    _ctx.builder->CreateBr(done_block);

    _ctx.set_insert_point(done_block);
    _ctx.builder->CreateRetVoid();

    return thunk;
}

llvm::Function *MemoryCodegen::get_or_create_realloc_thunk()
{
    if (llvm::Function *existing = _ctx.current_module()->getFunction(k_realloc_symbol)) {
        return existing;
    }

    llvm::Type *size_ty = _ctx.size_int_ty();
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);

    llvm::Function *thunk =
        declare_thunk(k_realloc_symbol, opaque_ptr, { opaque_ptr, size_ty }, { "block", "size" });

    // **`noalias` holds for a reseat too**, which is worth a sentence because `realloc` may hand back the
    // pointer it was given: the contract is that the old one is *dead* from that moment, so nothing the
    // caller may still legitimately hold overlaps the result. LLVM models the libc function itself the same
    // way. The byte count is argument 1 here, the block being argument 0
    mark_allocating_thunk(thunk, 1);

    llvm::Value *old_block = thunk->getArg(0);
    llvm::Value *size = thunk->getArg(1);

    llvm::Value *new_block = _ctx.builder->CreateCall(
        heap_realloc(), { old_block, size }, "block.new");

    _ctx.profile->emit_realloc(old_block, new_block, size);

    // **the one thunk where the delta is not readable off the arguments.** `realloc` is four operations
    // wearing one name, and only two of them move the count:
    //
    //   realloc(null, n)    succeeding   +1   it allocated
    //   realloc(p, n)       succeeding    0   one block became another, in place or moved
    //   realloc(p, 0)       -> null      -1   it freed
    //   realloc(p, n != 0)  -> null       0   **it failed, and p is still live**
    //
    // the last two are indistinguishable from the pointers alone, and the naive test - was live, is not
    // live now - reads the failure as a free. That does not merely mislabel one call: from the first
    // OOM onwards the count sits one low, so every leak after it reports as balanced
    llvm::BasicBlock *grew_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "realloc.grew", thunk);
    llvm::BasicBlock *shrank_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "realloc.shrank", thunk);
    llvm::BasicBlock *seated_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "realloc.seated", thunk);
    llvm::BasicBlock *released_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "realloc.released", thunk);
    llvm::BasicBlock *done_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "realloc.done", thunk);

    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(old_block), grew_block, shrank_block);

    // nothing came in. an allocation, if it produced anything
    _ctx.set_insert_point(grew_block);
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(new_block), done_block, seated_block);

    _ctx.set_insert_point(seated_block);
    gen_counter_delta(1);
    _ctx.builder->CreateBr(done_block);

    // something came in and nothing came back. a free only if a zero size asked for one - otherwise the
    // allocator refused and the caller still holds what it had
    _ctx.set_insert_point(shrank_block);
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateAnd(
            _ctx.builder->CreateIsNull(new_block),
            _ctx.builder->CreateIsNull(size),
            "realloc.gave_back"),
        released_block,
        done_block);

    _ctx.set_insert_point(released_block);
    gen_counter_delta(-1);
    _ctx.builder->CreateBr(done_block);

    _ctx.set_insert_point(done_block);
    _ctx.builder->CreateRet(new_block);

    return thunk;
}

llvm::Value *MemoryCodegen::gen_alloc(llvm::Value *size, const llvm::Twine &name)
{
    llvm::Value *n = _ctx.as_size(size);

    if (!_ctx.options.tracking_allocations()) {
        return _ctx.builder->CreateCall(heap_alloc(), { n }, name);
    }

    return _ctx.builder->CreateCall(get_or_create_alloc_thunk(), { n }, name);
}

llvm::Value *MemoryCodegen::gen_realloc(llvm::Value *block, llvm::Value *size, const llvm::Twine &name)
{
    llvm::Value *n = _ctx.as_size(size);

    // a literal null at the builtin. a loaded null still needs mem::realloc's
    // branch, or LLVM will not delete an allockind("realloc") pair - pinned by
    // tests_eco/memory/fixed_block_never_reaches_the_binary
    if (llvm::isa<llvm::ConstantPointerNull>(block)) {
        return gen_alloc(n, name);
    }

    if (!_ctx.options.tracking_allocations()) {
        return _ctx.builder->CreateCall(heap_realloc(), { block, n }, name);
    }

    return _ctx.builder->CreateCall(get_or_create_realloc_thunk(), { block, n }, name);
}

void MemoryCodegen::gen_free(llvm::Value *block)
{
    if (!_ctx.options.tracking_allocations()) {
        _ctx.builder->CreateCall(heap_free(), { block });
        return;
    }

    _ctx.builder->CreateCall(get_or_create_free_thunk(), { block });
}

llvm::Value *MemoryCodegen::gen_live_count(const llvm::Twine &name)
{
    return _ctx.builder->CreateLoad(
        llvm::Type::getInt64Ty(*_ctx.llvm_context), get_or_create_live_counter(), name);
}

void MemoryCodegen::gen_report()
{
    if (!_ctx.options.reporting_allocations()) {
        return;
    }

    // the `[section]` shape --print-symbol-table set and every diagnostic since has kept: a bracketed
    // header and two-space-indented detail, so the output is greppable and a golden can assert on it
    //
    // unnamed, for AbortCodegen::gen_abort's reason - a named global would be spun out to
    // memory.fmt.1, .2, ... and land in the module's symbol table once per copy
    llvm::Value *format = _ctx.builder->CreateGlobalStringPtr("[memory]\n  %llu live allocations\n", "");

    // no flush first, unlike the abort runtime: `printf` writes to the same buffered stdout `echo`
    // does, so the two are already in order, and this is the last thing the program prints
    _ctx.builder->CreateCall(
        _ctx.current_module()->getFunction("printf"), { format, gen_live_count("live") });
}

};
