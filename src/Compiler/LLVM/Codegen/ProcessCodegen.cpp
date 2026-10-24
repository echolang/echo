#include "Compiler/LLVM/Codegen/ProcessCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/RuntimeSymbols.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
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
    constexpr const char *k_hash_seed_symbol = "__eco_hash_seed";
    constexpr const char *k_seed_function = "__eco_seed_hash";

    // `_IONBF` as each C runtime spells it. the two disagree
    constexpr uint64_t k_ucrt_ionbf = 4;
    constexpr uint64_t k_wasi_libc_ionbf = 2;

    constexpr uint64_t k_ucrt_o_binary = 0x8000;
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

llvm::GlobalVariable *ProcessCodegen::get_or_create_hash_seed()
{
    llvm::GlobalVariable *seed =
        _ctx.get_or_create_odr_global(k_hash_seed_symbol, llvm::Type::getInt64Ty(*_ctx.llvm_context));

    // hide-except-export would make a lib's copy a different cell from the host's.
    // DLLExport is the keep-visible marker; a plugin then copies the host's value at load
    seed->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    return seed;
}

llvm::Value *ProcessCodegen::gen_hash_seed(const llvm::Twine &name)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    if (_ctx.options.fixed_hash_seed) {
        return llvm::ConstantInt::get(i64, 0);
    }

    return _ctx.builder->CreateLoad(i64, get_or_create_hash_seed(), name);
}

void ProcessCodegen::gen_seed_hash()
{
    if (!_ctx.program_hashes) {
        return;
    }

    // the cell has to exist even when the seed is fixed: a plugin dlsyms it. zero-init is 0
    get_or_create_hash_seed();

    if (_ctx.options.fixed_hash_seed) {
        return;
    }

    llvm::Module *module = _ctx.current_module();
    llvm::Function *seed = module->getFunction(k_seed_function);

    if (seed == nullptr) {
        llvm::Type *void_ty = llvm::Type::getVoidTy(*_ctx.llvm_context);
        llvm::Type *i8 = llvm::Type::getInt8Ty(*_ctx.llvm_context);
        llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
        llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
        llvm::Type *ptr = _ctx.opaque_ptr_type();
        llvm::Type *size_ty = _ctx.size_int_ty();

        seed = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, false), llvm::GlobalValue::LinkOnceODRLinkage, k_seed_function, module);
        seed->addFnAttr(llvm::Attribute::NoInline);
        seed->addFnAttr(llvm::Attribute::Cold);

        llvm::IRBuilderBase::InsertPointGuard restore(*_ctx.builder);
        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", seed);
        auto *env = llvm::BasicBlock::Create(*_ctx.llvm_context, "seed.env", seed);
        auto *check = llvm::BasicBlock::Create(*_ctx.llvm_context, "seed.check", seed);
        auto *parse = llvm::BasicBlock::Create(*_ctx.llvm_context, "seed.parse", seed);
        auto *draw = llvm::BasicBlock::Create(*_ctx.llvm_context, "seed.draw", seed);

        _ctx.builder->SetInsertPoint(entry);
        llvm::Value *slot = _ctx.builder->CreateAlloca(i64, nullptr, "seed.slot");
        _ctx.builder->CreateStore(llvm::ConstantInt::get(i64, 0), slot);

        // a lib's own cell is a different image. copy the host's seed when the main
        // module exported one (dynlib after main); otherwise draw, which is a C host
        if (_ctx.options.codegen.is_native_library()) {
            auto *copy = llvm::BasicBlock::Create(*_ctx.llvm_context, "seed.from_host", seed);
            llvm::Constant *sym =
                _ctx.builder->CreateGlobalStringPtr(k_hash_seed_symbol, ".eco.hash_seed_sym");
            llvm::Value *host = nullptr;

            switch (_ctx.options.codegen.host_symbol_lookup()) {
                case Compiler::HostSymbolLookup::t_get_proc_address: {
                    llvm::FunctionCallee module_handle = _ctx.libc_callee(
                        Compiler::RuntimeSymbol::t_get_module_handle_a, ptr, { ptr });
                    llvm::Value *main_image = _ctx.builder->CreateCall(
                        module_handle,
                        { llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)) },
                        "seed.main");
                    llvm::FunctionCallee proc = _ctx.libc_callee(
                        Compiler::RuntimeSymbol::t_get_proc_address, ptr, { ptr, ptr });
                    host = _ctx.builder->CreateCall(proc, { main_image, sym }, "seed.host");
                    break;
                }

                case Compiler::HostSymbolLookup::t_dlsym: {
                    llvm::Value *handle = _ctx.builder->CreateIntToPtr(
                        llvm::ConstantInt::getSigned(
                            _ctx.size_int_ty(), _ctx.options.codegen.rtld_default()),
                        ptr,
                        "seed.rtld");
                    llvm::FunctionCallee lookup =
                        _ctx.libc_callee(Compiler::RuntimeSymbol::t_dlsym, ptr, { ptr, ptr });
                    host = _ctx.builder->CreateCall(lookup, { handle, sym }, "seed.host");
                    break;
                }
            }

            _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(host), env, copy);

            _ctx.builder->SetInsertPoint(copy);
            _ctx.builder->CreateStore(
                _ctx.builder->CreateLoad(i64, host, "seed.copied"), get_or_create_hash_seed());
            _ctx.builder->CreateRetVoid();
        }
        else {
            _ctx.builder->CreateBr(env);
        }

        _ctx.builder->SetInsertPoint(env);
        llvm::Constant *name = _ctx.builder->CreateGlobalStringPtr("ECHO_HASH_SEED", ".eco.hash_seed_var");
        llvm::FunctionCallee getenv = _ctx.libc_callee(Compiler::RuntimeSymbol::t_getenv, ptr, { ptr });
        llvm::Value *spelled = _ctx.builder->CreateCall(getenv, { name }, "seed.env");
        _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(spelled), draw, check);

        // set but empty reads as unset, so `ECHO_HASH_SEED=` in a wrapper can undo a pin
        _ctx.builder->SetInsertPoint(check);
        llvm::Value *first = _ctx.builder->CreateLoad(i8, spelled, "seed.first");
        _ctx.builder->CreateCondBr(
            _ctx.builder->CreateICmpEQ(first, llvm::ConstantInt::get(i8, 0)), draw, parse);

        _ctx.builder->SetInsertPoint(parse);
        llvm::FunctionCallee strtoull = _ctx.libc_callee(Compiler::RuntimeSymbol::t_strtoull, i64, { ptr, ptr, i32 });
        llvm::Value *pinned = _ctx.builder->CreateCall(
            strtoull,
            { spelled, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), llvm::ConstantInt::get(i32, 10) },
            "seed.pinned");
        _ctx.builder->CreateStore(pinned, get_or_create_hash_seed());
        _ctx.builder->CreateRetVoid();

        _ctx.builder->SetInsertPoint(draw);

        switch (_ctx.options.codegen.entropy_source()) {
            case Compiler::EntropySource::t_arc4random: {
                llvm::FunctionCallee fill = _ctx.libc_callee(
                    Compiler::RuntimeSymbol::t_arc4random_buf, void_ty, { ptr, size_ty });
                _ctx.builder->CreateCall(fill, { slot, llvm::ConstantInt::get(size_ty, 8) });
                break;
            }

            case Compiler::EntropySource::t_getentropy: {
                // a failure leaves the zero stored above, and the address below still varies it
                llvm::FunctionCallee fill = _ctx.libc_callee(
                    Compiler::RuntimeSymbol::t_getentropy, i32, { ptr, size_ty });
                _ctx.builder->CreateCall(fill, { slot, llvm::ConstantInt::get(size_ty, 8) });
                break;
            }

            case Compiler::EntropySource::t_rand_s: {
                llvm::FunctionCallee fill = _ctx.libc_callee(Compiler::RuntimeSymbol::t_rand_s, i32, { ptr });
                _ctx.builder->CreateCall(fill, { slot });
                _ctx.builder->CreateCall(fill, { _ctx.builder->CreateConstGEP1_32(i8, slot, 4, "seed.high") });
                break;
            }
        }

        llvm::Value *drawn = _ctx.builder->CreateLoad(i64, slot, "seed.drawn");
        llvm::Value *where = _ctx.builder->CreatePtrToInt(slot, i64, "seed.where");
        _ctx.builder->CreateStore(_ctx.builder->CreateXor(drawn, where, "seed"), get_or_create_hash_seed());
        _ctx.builder->CreateRetVoid();
    }

    _ctx.builder->CreateCall(seed, {});
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

    // before any of the program's own statements, like the three stores above: a module-scope map is one
    gen_seed_hash();

    unbuffer_stdio();
    binary_stdio();
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

void ProcessCodegen::binary_stdio()
{
    if (!_ctx.options.codegen.is_windows()) {
        return;
    }

    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::FunctionCallee setmode = _ctx.libc_callee(Compiler::RuntimeSymbol::t_setmode, i32, { i32, i32 });

    for (unsigned fd : { 0u, 1u, 2u }) {
        _ctx.builder->CreateCall(
            setmode, { llvm::ConstantInt::get(i32, fd), llvm::ConstantInt::get(i32, k_ucrt_o_binary) });
    }
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
    // nothing to unbuffer and no seed to draw
    if (_ctx.options.codegen.stdio_unbuffering() == Compiler::StdioUnbuffering::t_none
        && (_ctx.options.fixed_hash_seed || !_ctx.program_hashes)) {
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

    // a library has no entry prologue, so the seed a host's first call into it hashes with is drawn here:
    // at `dlopen`, DLL attach or `_initialize`, before any export can run
    gen_seed_hash();
    unbuffer_stdio();
    binary_stdio();
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
