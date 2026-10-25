#include "Compiler/LLVM/Codegen/ProfileCodegen.h"

#include "AST/ASTCopy.h"
#include "AST/ASTFunctionEmission.h"
#include "AST/ASTFunctionProfile.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "Compiler/Fnv.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"

#include <eco_profile.h>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/CodeGen.h>

#include <string>
#include <vector>

namespace Compiler::LLVM
{

void ProfileCodegen::attach_location()
{
    llvm::BasicBlock *block = _ctx.builder->GetInsertBlock();
    if (block == nullptr) {
        return;
    }

    llvm::Function *fn = block->getParent();
    if (fn == nullptr) {
        return;
    }

    llvm::DISubprogram *sp = fn->getSubprogram();
    if (sp == nullptr) {
        return;
    }

    _ctx.builder->SetCurrentDebugLocation(
        llvm::DILocation::get(*_ctx.llvm_context, sp->getScopeLine(), 0, sp));
}

llvm::FunctionCallee ProfileCodegen::hook(
    const char *name,
    llvm::Type *return_type,
    llvm::ArrayRef<llvm::Type *> parameter_types
)
{
    return _ctx.current_module()->getOrInsertFunction(
        name, llvm::FunctionType::get(return_type, parameter_types, false));
}

llvm::Value *ProfileCodegen::as_i64(llvm::Value *value)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    if (value->getType() == i64) {
        return value;
    }
    return _ctx.builder->CreateZExtOrTrunc(value, i64);
}

llvm::StructType *ProfileCodegen::site_type()
{
    if (auto *existing = llvm::StructType::getTypeByName(*_ctx.llvm_context, "eco.prof.site")) {
        return existing;
    }

    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *ptr = _ctx.opaque_ptr_type();
    return llvm::StructType::create(*_ctx.llvm_context, { i32, i32, i64, ptr, ptr }, "eco.prof.site");
}

llvm::GlobalVariable *ProfileCodegen::site_constant(
    uint32_t kind,
    uint64_t type_bytes,
    const std::string &type_name,
    llvm::Function *function
)
{
    uint64_t digest = Compiler::k_fnv_offset_basis;
    digest = Compiler::fnv1a64(std::to_string(kind), digest);
    digest = Compiler::fnv1a64(std::to_string(type_bytes), digest);
    digest = Compiler::fnv1a64(type_name, digest);
    digest = Compiler::fnv1a64(function != nullptr ? function->getName().str() : std::string(), digest);
    const std::string name = "__eco_prof_site_" + Compiler::to_hex(digest);

    if (auto *existing = _ctx.current_module()->getGlobalVariable(name, true)) {
        return existing;
    }

    llvm::StructType *ty = site_type();
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::PointerType *ptr = llvm::cast<llvm::PointerType>(_ctx.opaque_ptr_type());

    llvm::Constant *name_const = _ctx.builder->CreateGlobalString(type_name, ".eco.prof.typename");
    llvm::Constant *fn_const = function != nullptr
        ? static_cast<llvm::Constant *>(function)
        : llvm::ConstantPointerNull::get(ptr);

    llvm::Constant *init = llvm::ConstantStruct::get(
        ty,
        {
            llvm::ConstantInt::get(i32, kind),
            llvm::ConstantInt::get(i32, 0),
            llvm::ConstantInt::get(i64, type_bytes),
            name_const,
            fn_const
        });

    return new llvm::GlobalVariable(
        *_ctx.current_module(),
        ty,
        /*isConstant=*/true,
        llvm::GlobalValue::LinkOnceODRLinkage,
        init,
        name);
}

void ProfileCodegen::mark_frame_pointers()
{
    if (!_ctx.options.profiling()) {
        return;
    }

    for (auto &unit : _ctx.cmp_units) {
        llvm::Module *mod = unit->llvm_module.get();
        if (mod == nullptr) {
            continue;
        }

        mod->setFramePointer(llvm::FramePointerKind::All);
        for (llvm::Function &fn : *mod) {
            if (fn.isDeclaration()) {
                continue;
            }
            fn.addFnAttr("frame-pointer", "all");
        }
    }
}

void ProfileCodegen::emit_start()
{
    if (!_ctx.options.profiling()) {
        return;
    }

    attach_location();
    _ctx.builder->CreateCall(hook("__eco_prof_start", llvm::Type::getVoidTy(*_ctx.llvm_context), {}));
}

void ProfileCodegen::emit_finish(int status)
{
    if (!_ctx.options.profiling()) {
        return;
    }

    attach_location();
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    _ctx.builder->CreateCall(
        hook("__eco_prof_finish", llvm::Type::getVoidTy(*_ctx.llvm_context), { i32 }),
        { llvm::ConstantInt::get(i32, status) });
}

void ProfileCodegen::instrument_main_returns(llvm::Function *main)
{
    if (!_ctx.options.profiling() || main == nullptr) {
        return;
    }

    std::vector<llvm::ReturnInst *> rets;
    for (llvm::BasicBlock &block : *main) {
        if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
            rets.push_back(ret);
        }
    }

    for (llvm::ReturnInst *ret : rets) {
        _ctx.set_insert_point(ret->getParent(), ret->getIterator());
        emit_finish(0);
    }
}

void ProfileCodegen::emit_alloc(llvm::Value *block, llvm::Value *size)
{
    if (!_ctx.options.profiling()) {
        return;
    }

    attach_location();
    llvm::Type *ptr = _ctx.opaque_ptr_type();
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    _ctx.builder->CreateCall(
        hook("__eco_prof_alloc", llvm::Type::getVoidTy(*_ctx.llvm_context), { ptr, i64 }),
        { block, as_i64(size) });
}

void ProfileCodegen::emit_realloc(llvm::Value *old_block, llvm::Value *block, llvm::Value *size)
{
    if (!_ctx.options.profiling()) {
        return;
    }

    attach_location();
    llvm::Type *ptr = _ctx.opaque_ptr_type();
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    _ctx.builder->CreateCall(
        hook("__eco_prof_realloc", llvm::Type::getVoidTy(*_ctx.llvm_context), { ptr, ptr, i64 }),
        { old_block, block, as_i64(size) });
}

void ProfileCodegen::emit_free(llvm::Value *block)
{
    if (!_ctx.options.profiling()) {
        return;
    }

    attach_location();
    llvm::Type *ptr = _ctx.opaque_ptr_type();
    _ctx.builder->CreateCall(
        hook("__eco_prof_free", llvm::Type::getVoidTy(*_ctx.llvm_context), { ptr }),
        { block });
}

void ProfileCodegen::emit_copy(
    uint32_t kind,
    const AST::ValueType &type,
    llvm::Function *function
)
{
    attach_location();
    llvm::Type *ty = _ctx.types->get_llvm_type(type, *_ctx.current_cmp_unit);
    const uint64_t bytes = _ctx.layout().getTypeAllocSize(ty);
    llvm::GlobalVariable *site = site_constant(
        kind, bytes, type.get_type_desciption(), function);
    llvm::Type *ptr = _ctx.opaque_ptr_type();
    _ctx.builder->CreateCall(
        hook("__eco_prof_copy", llvm::Type::getVoidTy(*_ctx.llvm_context), { ptr }),
        { site });
}

void ProfileCodegen::instrument_copy_function(llvm::Function &fn, const AST::FunctionDeclNode &decl)
{
    if (!_ctx.options.profiling()) {
        return;
    }

    if (!AST::is_copy_function(&decl)) {
        return;
    }

    llvm::BasicBlock *entry = &fn.getEntryBlock();
    _ctx.set_insert_point(entry, entry->getFirstNonPHIOrDbgOrAlloca());
    emit_copy(ECO_PROF_KIND_COPY_CTOR, decl.get_return_type(), &fn);
}

void ProfileCodegen::note_arrival(AST::ExprNode &node)
{
    if (!_ctx.options.profiling() || !node.bytewise_copy) {
        return;
    }

    if (AST::is_copy_function(_ctx.current_function)) {
        return;
    }

    const AST::ValueType type = node.result_type();
    if (!type.is_struct()
        && !type.is_enum()
        && !type.is_inline_array()
        && !type.is_wrapped_optional()) {
        return;
    }

    llvm::Function *fn = _ctx.builder->GetInsertBlock()->getParent();
    emit_copy(ECO_PROF_KIND_COPY_BYTES, type, fn);
}

void ProfileCodegen::instrument_body(llvm::Function &fn, const AST::FunctionDeclNode &decl)
{
    if (!_ctx.options.profiling() || !_ctx.options.profiling_calls()) {
        return;
    }

    if (!AST::profile_instruments(&decl, _ctx.options.profiling_stdlib())) {
        return;
    }

    llvm::BasicBlock *entry = &fn.getEntryBlock();
    _ctx.set_insert_point(entry, entry->getFirstNonPHIOrDbgOrAlloca());
    attach_location();

    llvm::GlobalVariable *site = site_constant(
        ECO_PROF_KIND_CALL, 0, decl.func_name(), &fn);
    llvm::Type *ptr = _ctx.opaque_ptr_type();
    llvm::Type *void_ty = llvm::Type::getVoidTy(*_ctx.llvm_context);
    _ctx.builder->CreateCall(hook("__eco_prof_enter", void_ty, { ptr }), { site });

    std::vector<llvm::ReturnInst *> rets;
    for (llvm::BasicBlock &block : fn) {
        if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
            rets.push_back(ret);
        }
    }

    llvm::FunctionCallee exit_fn = hook("__eco_prof_exit", void_ty, {});
    for (llvm::ReturnInst *ret : rets) {
        _ctx.set_insert_point(ret->getParent(), ret->getIterator());
        attach_location();
        _ctx.builder->CreateCall(exit_fn);
    }
}

};
