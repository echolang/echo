#include "Compiler/LLVM/Codegen/MemoryCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
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
};

llvm::Function *MemoryCodegen::declare_thunk(
    const std::string &name, llvm::Type *return_type,
    const std::vector<llvm::Type *> &parameter_types,
    const std::vector<const char *> &parameter_names)
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

    _ctx.builder->SetInsertPoint(llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", thunk));

    return thunk;
}

// **what we know about a thunk that hands back a block, said out loud.**
//
// `noalias` on the return is the claim that the block does not overlap anything the caller already holds,
// which is what an allocator *is* - and it is the one attribute in this compiler that is unambiguously
// sound to write by hand, because it is a property of `malloc`/`realloc` rather than of any Echo type. A
// borrow parameter gets nothing of the kind: the language has no exclusivity rule, and `$a->extend($a)` is
// a legal program in which two borrows name one object.
//
// LLVM already infers this whenever a pipeline runs - `malloc` is known to TargetLibraryInfo and
// FunctionAttrs propagates through the thunk. But **`echoc run` runs no such pass**: the JIT path is
// InternalizePass and GlobalDCEPass and nothing else, so without this the interpreter's memory has no
// aliasing information at all. Stating it is what makes the two paths agree.
//
// `allocsize` says which argument is the byte count, so the optimizer can fold a `size_of` query about the
// block. It is a *hint* with no soundness weight - a wrong index would mislead rather than miscompile -
// which is why it sits here beside `noalias` rather than needing an argument of its own
void MemoryCodegen::mark_allocating_thunk(llvm::Function *thunk, unsigned size_argument)
{
    thunk->addRetAttr(llvm::Attribute::NoAlias);
    thunk->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
        thunk->getContext(), size_argument, std::nullopt));
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

    // non-atomic, the shape ClassCodegen::gen_count_inc uses on a reference count and for the same
    // reason: the language has no threading model, so a count that could be raced is a count for a
    // language this is not
    llvm::Value *live = _ctx.builder->CreateLoad(i64, counter, "live");
    _ctx.builder->CreateStore(
        _ctx.builder->CreateAdd(live, llvm::ConstantInt::get(i64, delta), "live.next"), counter);
}

llvm::Function *MemoryCodegen::get_or_create_alloc_thunk()
{
    if (llvm::Function *existing = _ctx.current_module()->getFunction(k_alloc_symbol)) {
        return existing;
    }

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);

    llvm::Function *thunk = declare_thunk(k_alloc_symbol, opaque_ptr, { i64 }, { "size" });

    // the byte count is the only argument
    mark_allocating_thunk(thunk, 0);

    llvm::Value *block = _ctx.builder->CreateCall(
        _ctx.libc_callee("malloc", opaque_ptr, { i64 }), { thunk->getArg(0) }, "block");

    // **a failed allocation is not an allocation.** `mem::alloc` is documented to hand back null when
    // the allocator could not, so counting the attempt would leave a program that survived an OOM
    // reporting a leak it does not have
    llvm::BasicBlock *count_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "alloc.count", thunk);
    llvm::BasicBlock *done_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, "alloc.done", thunk);

    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(block), done_block, count_block);

    _ctx.builder->SetInsertPoint(count_block);
    gen_counter_delta(1);
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
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

    _ctx.builder->SetInsertPoint(count_block);
    gen_counter_delta(-1);
    _ctx.builder->CreateCall(_ctx.libc_callee("free", void_type, { opaque_ptr }), { block });
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
    _ctx.builder->CreateRetVoid();

    return thunk;
}

llvm::Function *MemoryCodegen::get_or_create_realloc_thunk()
{
    if (llvm::Function *existing = _ctx.current_module()->getFunction(k_realloc_symbol)) {
        return existing;
    }

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);

    llvm::Function *thunk =
        declare_thunk(k_realloc_symbol, opaque_ptr, { opaque_ptr, i64 }, { "block", "size" });

    // **`noalias` holds for a reseat too**, which is worth a sentence because `realloc` may hand back the
    // pointer it was given: the contract is that the old one is *dead* from that moment, so nothing the
    // caller may still legitimately hold overlaps the result. LLVM models the libc function itself the same
    // way. The byte count is argument 1 here, the block being argument 0
    mark_allocating_thunk(thunk, 1);

    llvm::Value *old_block = thunk->getArg(0);
    llvm::Value *size = thunk->getArg(1);

    llvm::Value *new_block = _ctx.builder->CreateCall(
        _ctx.libc_callee("realloc", opaque_ptr, { opaque_ptr, i64 }),
        { old_block, size }, "block.new");

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
    _ctx.builder->SetInsertPoint(grew_block);
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(new_block), done_block, seated_block);

    _ctx.builder->SetInsertPoint(seated_block);
    gen_counter_delta(1);
    _ctx.builder->CreateBr(done_block);

    // something came in and nothing came back. a free only if a zero size asked for one - otherwise the
    // allocator refused and the caller still holds what it had
    _ctx.builder->SetInsertPoint(shrank_block);
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateAnd(
            _ctx.builder->CreateIsNull(new_block),
            _ctx.builder->CreateIsNull(size),
            "realloc.gave_back"),
        released_block,
        done_block);

    _ctx.builder->SetInsertPoint(released_block);
    gen_counter_delta(-1);
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
    _ctx.builder->CreateRet(new_block);

    return thunk;
}

llvm::Value *MemoryCodegen::gen_alloc(llvm::Value *size, const llvm::Twine &name)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    // with tracking off the seam is not a layer, it *is* the call - so an untracked build emits exactly
    // what it emitted before this subsystem existed, down to the symbol
    if (!_ctx.options.tracking_allocations()) {
        return _ctx.builder->CreateCall(
            _ctx.libc_callee("malloc", _ctx.opaque_ptr_type(), { i64 }), { size }, name);
    }

    return _ctx.builder->CreateCall(get_or_create_alloc_thunk(), { size }, name);
}

llvm::Value *MemoryCodegen::gen_realloc(llvm::Value *block, llvm::Value *size, const llvm::Twine &name)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    if (!_ctx.options.tracking_allocations()) {
        return _ctx.builder->CreateCall(
            _ctx.libc_callee("realloc", opaque_ptr, { opaque_ptr, i64 }), { block, size }, name);
    }

    return _ctx.builder->CreateCall(get_or_create_realloc_thunk(), { block, size }, name);
}

void MemoryCodegen::gen_free(llvm::Value *block)
{
    llvm::Type *void_type = llvm::Type::getVoidTy(*_ctx.llvm_context);

    if (!_ctx.options.tracking_allocations()) {
        _ctx.builder->CreateCall(
            _ctx.libc_callee("free", void_type, { _ctx.opaque_ptr_type() }), { block });
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
