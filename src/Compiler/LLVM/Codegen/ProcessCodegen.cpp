#include "Compiler/LLVM/Codegen/ProcessCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>

#include <cassert>

namespace Compiler::LLVM
{

namespace
{
    // the emitted runtime's symbols, spelled once. an IR check names them, so they are part of the
    // compiler's observable surface rather than an implementation detail
    constexpr const char *k_argc_symbol = "__eco_argc";
    constexpr const char *k_argv_symbol = "__eco_argv";
    constexpr const char *k_envp_symbol = "__eco_envp";
};

llvm::GlobalVariable *ProcessCodegen::get_or_create_argc()
{
    return _ctx.get_or_create_odr_global(k_argc_symbol, llvm::Type::getInt64Ty(*_ctx.llvm_context));
}

llvm::GlobalVariable *ProcessCodegen::get_or_create_argv()
{
    return _ctx.get_or_create_odr_global(k_argv_symbol, _ctx.opaque_ptr_type());
}

llvm::GlobalVariable *ProcessCodegen::get_or_create_envp()
{
    return _ctx.get_or_create_odr_global(k_envp_symbol, _ctx.opaque_ptr_type());
}

void ProcessCodegen::gen_capture(llvm::Function *entry)
{
    assert(entry->arg_size() == 3
        && "the entry point takes argc, argv and envp - a capture over anything else is a mistake");

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    // widened here rather than at every read, so `usize` is what the global holds
    _ctx.builder->CreateStore(
        _ctx.builder->CreateSExt(entry->getArg(0), i64, "argc.widened"),
        get_or_create_argc());

    _ctx.builder->CreateStore(entry->getArg(1), get_or_create_argv());
    _ctx.builder->CreateStore(entry->getArg(2), get_or_create_envp());

    _ctx.emit_unbuffer_stdio();
}

llvm::Value *ProcessCodegen::gen_argc(const llvm::Twine &name)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    return _ctx.builder->CreateLoad(i64, get_or_create_argc(), name);
}

llvm::Value *ProcessCodegen::gen_argv(const llvm::Twine &name)
{
    return _ctx.builder->CreateLoad(_ctx.opaque_ptr_type(), get_or_create_argv(), name);
}

llvm::Value *ProcessCodegen::gen_envp(const llvm::Twine &name)
{
    return _ctx.builder->CreateLoad(_ctx.opaque_ptr_type(), get_or_create_envp(), name);
}

};
