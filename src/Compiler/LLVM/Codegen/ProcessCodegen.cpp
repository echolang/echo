#include "Compiler/LLVM/Codegen/ProcessCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/RuntimeSymbols.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

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

    // `_IONBF` as each C runtime spells it. the two disagree
    constexpr uint64_t k_ucrt_ionbf = 4;
    constexpr uint64_t k_wasi_libc_ionbf = 2;
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
    assert(entry->arg_size() == _ctx.options.codegen.entry_arg_count()
        && "the entry point was declared with the row's own argument count");

    llvm::Type *size_ty = _ctx.size_int_ty();
    llvm::Value *argc = entry->getArg(0);
    if (argc->getType() != size_ty) {
        argc = _ctx.builder->CreateSExt(argc, size_ty, "argc.widened");
    }

    // widened here rather than at every read, so `usize` is what the global holds
    _ctx.builder->CreateStore(argc, get_or_create_argc());

    _ctx.builder->CreateStore(entry->getArg(1), get_or_create_argv());

    if (_ctx.options.codegen.entry_receives_envp()) {
        _ctx.builder->CreateStore(entry->getArg(2), get_or_create_envp());
    }
    else {
        store_platform_envp();
    }

    unbuffer_stdio();
}

llvm::Value *ProcessCodegen::load_platform_envp()
{
    llvm::Type *ptr = _ctx.opaque_ptr_type();

    switch (_ctx.options.codegen.environ_source()) {
        case Compiler::EnvironSource::t_wasilibc_get_environ: {
            llvm::FunctionCallee get = _ctx.libc_callee(
                Compiler::RuntimeSymbol::t_wasilibc_get_environ, ptr, {});
            return _ctx.builder->CreateCall(get, {}, "environ");
        }

        case Compiler::EnvironSource::t_ns_get_environ: {
            llvm::FunctionCallee get = _ctx.libc_callee(
                Compiler::RuntimeSymbol::t_ns_get_environ, ptr, {});
            llvm::Value *slot = _ctx.builder->CreateCall(get, {}, "environ.slot");
            return _ctx.builder->CreateLoad(ptr, slot, "environ");
        }

        case Compiler::EnvironSource::t_none:
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr));

        case Compiler::EnvironSource::t_libc_environ: {
            llvm::GlobalVariable *environ_gv =
                _ctx.libc_global(Compiler::RuntimeSymbol::t_environ, ptr);
            return _ctx.builder->CreateLoad(ptr, environ_gv, "environ");
        }
    }

    assert(false && "unhandled EnvironSource in load_platform_envp");
    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr));
}

void ProcessCodegen::store_platform_envp()
{
    _ctx.builder->CreateStore(load_platform_envp(), get_or_create_envp());
}

void ProcessCodegen::unbuffer_stdio()
{
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *ptr = _ctx.opaque_ptr_type();

    switch (_ctx.options.codegen.stdio_unbuffering()) {
        case Compiler::StdioUnbuffering::t_none:
            return;

        case Compiler::StdioUnbuffering::t_ucrt_stdout_stderr:
            unbuffer_ucrt(i32, ptr);
            return;

        case Compiler::StdioUnbuffering::t_wasi_libc_stdout:
            unbuffer_wasi_libc(i32, ptr);
            return;
    }

    assert(false && "unhandled StdioUnbuffering in unbuffer_stdio");
}

void ProcessCodegen::unbuffer_ucrt(llvm::Type *i32, llvm::Type *ptr)
{
    // stdout is fully buffered when it is a pipe. `echo` of a string goes through
    // `_write` (unbuffered) after `fflush(NULL)`, and that fflush is a no-op from
    // JIT'd code on Windows: MCJIT resolves UCRT but `fflush(NULL)` does not drain
    // this process's FILE*. unbuffering stdout and stderr makes `printf` and
    // `_write` the same kind of write, so program order is what the goldens record
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::FunctionCallee iob = _ctx.libc_callee(
        Compiler::RuntimeSymbol::t_acrt_iob_func, ptr, { i32 });
    llvm::FunctionCallee setvbuf_fn = _ctx.libc_callee(
        Compiler::RuntimeSymbol::t_setvbuf, i32, { ptr, ptr, i32, i64 });
    llvm::Value *null = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ptr));
    llvm::Value *ionbf = llvm::ConstantInt::get(i32, k_ucrt_ionbf);
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);

    for (unsigned fd : { 1u, 2u }) {
        llvm::Value *file = _ctx.builder->CreateCall(
            iob, { llvm::ConstantInt::get(i32, fd) });
        _ctx.builder->CreateCall(setvbuf_fn, { file, null, ionbf, zero });
    }
}

void ProcessCodegen::unbuffer_wasi_libc(llvm::Type *i32, llvm::Type *ptr)
{
    llvm::GlobalVariable *stdout_gv = _ctx.libc_global(Compiler::RuntimeSymbol::t_stdout, ptr);

    llvm::FunctionCallee setvbuf_fn = _ctx.libc_callee(
        Compiler::RuntimeSymbol::t_setvbuf,
        i32,
        { ptr, ptr, i32, _ctx.size_int_ty() });

    llvm::Value *file = _ctx.builder->CreateLoad(ptr, stdout_gv, "stdout");
    llvm::Value *null = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ptr));
    llvm::Value *ionbf = llvm::ConstantInt::get(i32, k_wasi_libc_ionbf);
    llvm::Value *zero = llvm::ConstantInt::get(_ctx.size_int_ty(), 0);
    _ctx.builder->CreateCall(setvbuf_fn, { file, null, ionbf, zero });
}

void ProcessCodegen::gen_startup()
{
    // nothing to unbuffer
    if (_ctx.options.codegen.stdio_unbuffering() == Compiler::StdioUnbuffering::t_none) {
        return;
    }

    llvm::Module *module = _ctx.current_module();
    constexpr const char *k_name = "__eco_startup";

    if (module->getFunction(k_name) != nullptr) {
        return;
    }

    llvm::Function *fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false),
        llvm::GlobalValue::InternalLinkage,
        k_name,
        module);

    llvm::IRBuilderBase::InsertPointGuard restore(*_ctx.builder);
    _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
    _ctx.builder->SetInsertPoint(
        llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn));

    unbuffer_stdio();
    _ctx.builder->CreateRetVoid();

    llvm::appendToGlobalCtors(*module, fn, 65535);
}

void ProcessCodegen::gen_library_lifecycle(llvm::Function *fini_walker)
{
    gen_startup();

    // null when nothing is owed a teardown
    if (fini_walker == nullptr) {
        return;
    }

    llvm::Module *module = _ctx.current_module();
    constexpr const char *k_name = "__eco_lib_fini";

    if (module->getFunction(k_name) != nullptr) {
        return;
    }

    llvm::Function *fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false),
        llvm::GlobalValue::LinkOnceODRLinkage,
        k_name,
        module);

    llvm::IRBuilderBase::InsertPointGuard restore(*_ctx.builder);
    _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
    _ctx.builder->SetInsertPoint(
        llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn));

    _ctx.builder->CreateCall(fini_walker);
    _ctx.builder->CreateRetVoid();

    if (_ctx.targeting_windows()) {
        // COFF `.CRT$XTX` (llvm.global_dtors) runs at process exit, after the
        // host has printed and often after stdout is gone. `atexit` in a DLL
        // is the CRT's module-local onexit table; FreeLibrary walks it.
        constexpr const char *k_reg = "__eco_lib_atexit";

        if (module->getFunction(k_reg) == nullptr) {
            llvm::Function *reg = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false),
                llvm::GlobalValue::InternalLinkage,
                k_reg,
                module);

            llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
            llvm::FunctionCallee atexit = _ctx.libc_callee(
                Compiler::RuntimeSymbol::t_atexit, i32, { _ctx.opaque_ptr_type() });

            _ctx.builder->SetInsertPoint(
                llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", reg));
            _ctx.builder->CreateCall(atexit, { fn });
            _ctx.builder->CreateRetVoid();
            llvm::appendToGlobalCtors(*module, reg, 65535);
        }
    }
    else {
        llvm::appendToGlobalDtors(*module, fn, 65535);
    }
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
    if (_ctx.options.codegen.is_library()) {
        llvm::Value *envp = load_platform_envp();
        envp->setName(name);
        return envp;
    }

    return _ctx.builder->CreateLoad(_ctx.opaque_ptr_type(), get_or_create_envp(), name);
}

};
