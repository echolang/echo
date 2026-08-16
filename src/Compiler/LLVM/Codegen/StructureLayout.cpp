#include "Compiler/LLVM/Codegen/StructureLayout.h"

#include "AST/ASTValueType.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MathExtras.h>

#include <fmt/core.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace Compiler::LLVM
{

namespace
{
    llvm::Type *alignment_unit_type(llvm::LLVMContext &context, uint64_t align)
    {
        switch (align) {
            case 1:  return llvm::Type::getInt8Ty(context);
            case 2:  return llvm::Type::getInt16Ty(context);
            case 4:  return llvm::Type::getInt32Ty(context);
            case 8:  return llvm::Type::getInt64Ty(context);
            case 16: return llvm::Type::getInt128Ty(context);
        }

        llvm_unreachable("ABI alignment is a power of two in 1..16");
    }

    bool enum_has_payload(const AST::ComplexType &type)
    {
        for (const AST::ComplexType::EnumCase &entry : type.enum_cases()) {
            if (entry.has_payload()) {
                return true;
            }
        }

        return false;
    }

    llvm::Type *require_lowered(
        const std::function<llvm::Type *(const AST::ValueType &)> &lower,
        const AST::ValueType &type,
        const AST::ComplexType &owner,
        size_t property_index,
        CodegenContext &ctx
    )
    {
        llvm::Type *llvm_type = lower(type);
        if (llvm_type != nullptr) {
            return llvm_type;
        }

        assert(false);
        throw ctx.error(fmt::format(
            "Unknown type for field '{}' in struct '{}'.",
            owner.get_property(property_index).name, owner.name.value_or("<anonymous>")
        ));
    }
}

void fill_structure_body(
    structure_id_t struct_id,
    const AST::ComplexType &type,
    StructureTable &table,
    llvm::LLVMContext &context,
    const llvm::DataLayout &layout,
    CodegenContext &ctx,
    const std::function<llvm::Type *(const AST::ValueType &)> &lower
)
{
    auto body_of = [&]() -> Structure & {
        return table.get_structure(struct_id);
    };

    assert(body_of().llvm_struct != nullptr);

    // a payload enum overlays its cases in one storage region. the AST properties stay one field
    // per payload slot - that is what classify_copy and the per-case drop still fold over - and
    // only this LLVM type is packed. a `[N x i8]` *property* would fold to t_bytes
    if (type.is_enum_kind() && enum_has_payload(type)) {
        llvm::Type *tag_ty = require_lowered(
            lower, type.get_property_type(AST::k_enum_tag_index), type, AST::k_enum_tag_index, ctx);

        std::vector<uint64_t> relative(type.property_count(), 0);
        uint64_t max_size = 0;
        uint64_t max_align = 1;

        for (const AST::ComplexType::EnumCase &entry : type.enum_cases()) {
            if (!entry.has_payload()) {
                continue;
            }

            std::vector<llvm::Type *> field_types;
            field_types.reserve(entry.payload_field_count);

            for (size_t i = 0; i < entry.payload_field_count; i++) {
                const size_t prop_i = entry.first_payload_property + i;
                field_types.push_back(require_lowered(
                    lower, type.get_property_type(prop_i), type, prop_i, ctx));
            }

            // a throwaway struct of this case's fields, so the DataLayout is the one answer
            // to where each field sits and how wide the case is. a hand-rolled align_up
            // would be a second layout, and the two would drift
            llvm::StructType *case_ty = llvm::StructType::get(context, field_types);
            const llvm::StructLayout *case_layout = layout.getStructLayout(case_ty);

            for (size_t i = 0; i < entry.payload_field_count; i++) {
                relative[entry.first_payload_property + i] = case_layout->getElementOffset(i);
            }

            max_size = std::max<uint64_t>(max_size, layout.getTypeAllocSize(case_ty));
            max_align = std::max<uint64_t>(max_align, case_layout->getAlignment().value());
        }

        Structure &packed = body_of();
        packed.packed_payload = true;
        packed.property_byte_offset.assign(type.property_count(), 0);

        // a payload of only zero-sized fields still overlays: the AST has slots, the LLVM
        // type is just the tag, and the offset table names the one-past-the-tag address a
        // zero-sized field lives at. skipping the table here made gep_property CreateStructGEP
        // a missing LLVM field
        if (max_size == 0) {
            packed.llvm_struct->setBody(std::vector<llvm::Type *>{ tag_ty });

            const uint64_t payload_start = layout.getTypeAllocSize(packed.llvm_struct);
            for (size_t i = 1; i < type.property_count(); i++) {
                packed.property_byte_offset[i] = payload_start + relative[i];
            }

            return;
        }

        const uint64_t storage_size = llvm::alignTo(max_size, max_align);
        const uint64_t units = storage_size / max_align;
        llvm::Type *storage_ty = llvm::ArrayType::get(alignment_unit_type(context, max_align), units);

        packed.llvm_struct->setBody({ tag_ty, storage_ty });

        const uint64_t payload_start = layout.getStructLayout(packed.llvm_struct)->getElementOffset(1);
        for (size_t i = 1; i < type.property_count(); i++) {
            packed.property_byte_offset[i] = payload_start + relative[i];
        }

        return;
    }

    std::vector<llvm::Type *> member_types;
    member_types.reserve(type.property_count());

    for (size_t i = 0; i < type.property_count(); i++) {
        member_types.push_back(require_lowered(lower, type.get_property_type(i), type, i, ctx));
    }

    Structure &body = body_of();
    body.llvm_struct->setBody(member_types);
    body.packed_payload = false;
    body.property_byte_offset.assign(type.property_count(), 0);

    if (type.property_count() == 0) {
        return;
    }

    const llvm::StructLayout *sl = layout.getStructLayout(body.llvm_struct);
    for (size_t i = 0; i < type.property_count(); i++) {
        body.property_byte_offset[i] = sl->getElementOffset(i);
    }
}

};
