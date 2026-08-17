#include "Compiler/LLVM/Codegen/CountAccess.h"

#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTAtomicity.h"

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace Compiler::LLVM
{

CountAccess count_access_for(const AST::ComplexType *complex)
{
    if (complex == nullptr) {
        return CountAccess::t_atomic;
    }

    return AST::counts_are_atomic(*complex) ? CountAccess::t_atomic : CountAccess::t_plain;
}

llvm::Value *counts_are_atomic_flag(
    CodegenContext &ctx,
    llvm::Value *handle,
    llvm::Type *box_type
)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*ctx.llvm_context);
    llvm::Type *info_type = ctx.types->typeinfo_llvm_type();
    llvm::Value *typeinfo = ctx.builder->CreateLoad(
        ctx.opaque_ptr_type(),
        ctx.builder->CreateStructGEP(
            box_type, handle, ClassBox::typeinfo_index, "typeinfo_ptr"),
        "typeinfo");
    llvm::Value *flags = ctx.builder->CreateLoad(
        i64,
        ctx.builder->CreateStructGEP(
            info_type, typeinfo, ClassTypeInfo::flags_index, "flags_ptr"),
        "flags");
    return ctx.builder->CreateICmpNE(
        ctx.builder->CreateAnd(
            flags, llvm::ConstantInt::get(i64, ClassTypeInfo::atomic_flag), "atomic.bit"),
        llvm::ConstantInt::get(i64, 0),
        "counts_are_atomic");
}

llvm::Value *apply_count_access(
    CodegenContext &ctx,
    CountAccess access,
    llvm::Value *handle,
    llvm::Type *box_type,
    const char *label,
    llvm::function_ref<llvm::Value *(bool atomic)> emit
)
{
    if (access != CountAccess::t_from_typeinfo) {
        return emit(access == CountAccess::t_atomic);
    }

    llvm::Function *function = ctx.builder->GetInsertBlock()->getParent();
    llvm::BasicBlock *atomic_block =
        llvm::BasicBlock::Create(*ctx.llvm_context, std::string(label) + ".atomic", function);
    llvm::BasicBlock *plain_block =
        llvm::BasicBlock::Create(*ctx.llvm_context, std::string(label) + ".plain", function);
    llvm::BasicBlock *join =
        llvm::BasicBlock::Create(*ctx.llvm_context, std::string(label) + ".join", function);

    ctx.builder->CreateCondBr(
        counts_are_atomic_flag(ctx, handle, box_type), atomic_block, plain_block);

    ctx.set_insert_point(atomic_block);
    llvm::Value *atomic_value = emit(true);
    llvm::BasicBlock *atomic_end = ctx.builder->GetInsertBlock();
    ctx.builder->CreateBr(join);

    ctx.set_insert_point(plain_block);
    llvm::Value *plain_value = emit(false);
    llvm::BasicBlock *plain_end = ctx.builder->GetInsertBlock();
    ctx.builder->CreateBr(join);

    ctx.set_insert_point(join);

    if (atomic_value == nullptr) {
        return nullptr;
    }

    llvm::PHINode *joined = ctx.builder->CreatePHI(
        atomic_value->getType(), 2, std::string(label) + ".next");
    joined->addIncoming(atomic_value, atomic_end);
    joined->addIncoming(plain_value, plain_end);
    return joined;
}

};
