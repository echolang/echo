#include "Compiler/LLVM/Codegen/ProcessCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

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
    constexpr const char *k_argc_symbol = "__eco_argc";
    constexpr const char *k_argv_symbol = "__eco_argv";
    constexpr const char *k_envp_symbol = "__eco_envp";
};

llvm::GlobalVariable *ProcessCodegen::get_or_create_argc()
{
    return _ctx.get_or_create_odr_global(k_argc_symbol, _ctx.size_int_ty());
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
    assert((entry->arg_size() == 3 || entry->arg_size() == 2)
        && "the entry point takes argc and argv, and envp on native");

    llvm::Type *size_ty = _ctx.size_int_ty();
    llvm::Value *argc = entry->getArg(0);
    if (argc->getType() != size_ty) {
        argc = _ctx.builder->CreateSExt(argc, size_ty, "argc.widened");
    }

    // widened here rather than at every read, so `usize` is what the global holds
    _ctx.builder->CreateStore(argc, get_or_create_argc());

    _ctx.builder->CreateStore(entry->getArg(1), get_or_create_argv());

    if (entry->arg_size() == 3) {
        _ctx.builder->CreateStore(entry->getArg(2), get_or_create_envp());
    }
    else {
        _ctx.builder->CreateStore(
            llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(_ctx.opaque_ptr_type())),
            get_or_create_envp());
    }

    _ctx.emit_unbuffer_stdio();
}

llvm::Value *ProcessCodegen::gen_argc(const llvm::Twine &name)
{
    return _ctx.builder->CreateLoad(_ctx.size_int_ty(), get_or_create_argc(), name);
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
