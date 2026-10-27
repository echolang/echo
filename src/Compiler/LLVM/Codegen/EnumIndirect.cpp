#include "Compiler/LLVM/Codegen/EnumIndirect.h"

#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/Codegen/MemoryCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/SymbolTable.h"

#include "AST/ASTValueType.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <cassert>

namespace Compiler::LLVM
{

namespace
{
    llvm::Value *overlay_pointer_slot(CodegenContext &ctx, const Structure &structure, llvm::Value *enum_base)
    {
        llvm::Type *i8 = llvm::Type::getInt8Ty(*ctx.llvm_context);
        llvm::Value *offset = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(*ctx.llvm_context), structure.packed_payload_offset);

        return ctx.builder->CreateGEP(i8, enum_base, offset, "enum.cell.slot");
    }

    llvm::Value *load_overlay_pointer(
        CodegenContext &ctx,
        const Structure &structure,
        llvm::Value *enum_base
    )
    {
        llvm::Type *ptr_ty = llvm::PointerType::get(*ctx.llvm_context, 0);
        llvm::Value *slot = overlay_pointer_slot(ctx, structure, enum_base);
        llvm::LoadInst *load = ctx.builder->CreateLoad(ptr_ty, slot, "enum.cell");

        // the overlay shares bytes with sibling cases, so this load carries no !tbaa
        return load;
    }

    void store_overlay_pointer(
        CodegenContext &ctx,
        const Structure &structure,
        llvm::Value *enum_base,
        llvm::Value *cell
    )
    {
        llvm::Value *slot = overlay_pointer_slot(ctx, structure, enum_base);
        ctx.builder->CreateStore(cell, slot);
    }

    llvm::Value *allocate_cell_of(CodegenContext &ctx, llvm::StructType *cell_ty)
    {
        llvm::Type *i64 = llvm::Type::getInt64Ty(*ctx.llvm_context);
        const uint64_t bytes = std::max<uint64_t>(ctx.layout().getTypeAllocSize(cell_ty), 1);
        llvm::Value *fresh = ctx.memory->gen_alloc(llvm::ConstantInt::get(i64, bytes), "enum.cell.new");

        ctx.abort->gen_cold_abort_if(
            ctx.builder->CreateIsNull(fresh, "enum.cell.oom"),
            "out of memory",
            "allocating an indirect enum case",
            "enum_indirect_oom");

        ctx.builder->CreateMemSet(
            fresh,
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(*ctx.llvm_context), 0),
            llvm::ConstantInt::get(i64, bytes),
            llvm::MaybeAlign(ctx.layout().getABITypeAlign(cell_ty)));

        return fresh;
    }
}

void EnumIndirect::alloc_cell(
    CodegenContext &ctx,
    const Structure &structure,
    const LValue &enum_base
)
{
    const AST::ComplexType *ct = enum_base.storage_type.get_complex_type();
    assert(ct != nullptr && ct->is_enum_kind());

    llvm::Type *tag_ty = structure.llvm_struct->getElementType(0);
    llvm::Value *tag_ptr = ctx.builder->CreateStructGEP(
        structure.llvm_struct, enum_base.address, 0, "enum.tag.ptr");
    llvm::Value *tag = ctx.builder->CreateLoad(tag_ty, tag_ptr, "enum.tag");

    llvm::Function *function = ctx.builder->GetInsertBlock()->getParent();
    llvm::BasicBlock *done =
        llvm::BasicBlock::Create(*ctx.llvm_context, "enum.cell.alloc.done", function);

    for (const AST::ComplexType::EnumCase &entry : ct->enum_cases()) {
        if (!AST::enum_payload_is_heap(entry)) {
            continue;
        }

        assert(entry.ordinal < structure.enum_heap_cell.size());
        llvm::StructType *cell_ty = structure.enum_heap_cell[entry.ordinal];
        assert(cell_ty != nullptr);

        llvm::BasicBlock *alloc_block =
            llvm::BasicBlock::Create(*ctx.llvm_context, "enum.cell.alloc.case", function);
        llvm::BasicBlock *next =
            llvm::BasicBlock::Create(*ctx.llvm_context, "enum.cell.alloc.next", function);

        llvm::Value *want = llvm::ConstantInt::get(tag_ty, static_cast<uint64_t>(entry.discriminant));
        ctx.builder->CreateCondBr(
            ctx.builder->CreateICmpEQ(tag, want, "enum.cell.case"), alloc_block, next);

        ctx.set_insert_point(alloc_block);
        store_overlay_pointer(ctx, structure, enum_base.address, allocate_cell_of(ctx, cell_ty));
        ctx.builder->CreateBr(done);

        ctx.set_insert_point(next);
    }

    ctx.builder->CreateBr(done);
    ctx.set_insert_point(done);
}

llvm::Value *EnumIndirect::load_cell(
    CodegenContext &ctx,
    const Structure &structure,
    const LValue &enum_base,
    size_t property_index
)
{
    assert(structure.property_lives_in_heap(property_index));

    llvm::Value *cell = load_overlay_pointer(ctx, structure, enum_base.address);

    if (ctx.options.assertions_enabled()) {
        ctx.abort->gen_cold_abort_if(
            ctx.builder->CreateIsNull(cell, "enum.cell.missing"),
            "indirect enum cell is missing",
            "the case constructor plants the allocation",
            "enum_indirect_missing");
    }

    return cell;
}

void EnumIndirect::free_cell(
    CodegenContext &ctx,
    const Structure &structure,
    const LValue &enum_base
)
{
    llvm::Value *cell = load_overlay_pointer(ctx, structure, enum_base.address);
    ctx.memory->gen_free(cell);

    store_overlay_pointer(
        ctx,
        structure,
        enum_base.address,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(*ctx.llvm_context, 0)));
}

};
