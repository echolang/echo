#include "Compiler/LLVM/Codegen/StructureLayout.h"

#include "AST/ASTValueType.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MathExtras.h>

#include <fmt/core.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
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

        return nullptr;
    }

    llvm::Type *lowered_field(
        const std::function<llvm::Type *(const AST::ValueType &)> &lower,
        const AST::ValueType &type
    )
    {
        llvm::Type *llvm_type = lower(type);
        if (llvm_type == nullptr || llvm_type->isVoidTy()) {
            return nullptr;
        }

        return llvm_type;
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

    llvm::Type *named_fat_pointer(llvm::LLVMContext &context, const char *name)
    {
        llvm::Type *ptr = llvm::PointerType::get(context, 0);
        if (auto *existing = llvm::StructType::getTypeByName(context, name)) {
            return existing;
        }

        return llvm::StructType::create(context, { ptr, ptr }, name);
    }

    llvm::Type *primitive_llvm_type(
        llvm::LLVMContext &context,
        const llvm::DataLayout &layout,
        AST::ValueTypePrimitive primitive
    )
    {
        switch (primitive) {
        case AST::ValueTypePrimitive::t_void:
            return llvm::Type::getVoidTy(context);
        case AST::ValueTypePrimitive::t_float32:
            return llvm::Type::getFloatTy(context);
        case AST::ValueTypePrimitive::t_float64:
            return llvm::Type::getDoubleTy(context);
        case AST::ValueTypePrimitive::t_int8:
        case AST::ValueTypePrimitive::t_uint8:
            return llvm::Type::getInt8Ty(context);
        case AST::ValueTypePrimitive::t_int16:
        case AST::ValueTypePrimitive::t_uint16:
            return llvm::Type::getInt16Ty(context);
        case AST::ValueTypePrimitive::t_int32:
        case AST::ValueTypePrimitive::t_uint32:
            return llvm::Type::getInt32Ty(context);
        case AST::ValueTypePrimitive::t_int64:
        case AST::ValueTypePrimitive::t_uint64:
            return llvm::Type::getInt64Ty(context);
        case AST::ValueTypePrimitive::t_usize:
        case AST::ValueTypePrimitive::t_isize:
            return layout.getIntPtrType(context);
        case AST::ValueTypePrimitive::t_bool:
            return llvm::Type::getInt1Ty(context);
        default:
            return nullptr;
        }
    }
}

llvm::Type *llvm_type_of(
    const AST::ValueType &type,
    llvm::LLVMContext &context,
    const llvm::DataLayout &layout,
    const std::function<llvm::Type *(const AST::ComplexType *)> &aggregate
)
{
    if (type.is_pointer() || type.is_class() || type.is_weak() || type.is_c_function()) {
        return llvm::PointerType::get(context, 0);
    }

    if (type.is_callable()) {
        return named_fat_pointer(context, "eco.callable");
    }

    if (type.is_interface()) {
        return named_fat_pointer(context, "eco.iface");
    }

    if (type.is_primitive()) {
        return primitive_llvm_type(context, layout, type.get_primitive_type());
    }

    if (type.is_opaque()) {
        return nullptr;
    }

    if (type.is_struct() || type.is_enum()) {
        return aggregate(type.get_complex_type());
    }

    if (type.is_inline_array()) {
        const std::optional<uint64_t> length = type.bound_array_length();
        if (!length.has_value()) {
            return nullptr;
        }

        llvm::Type *element = llvm_type_of(type.array_element(), context, layout, aggregate);
        if (element == nullptr) {
            return nullptr;
        }

        return llvm::ArrayType::get(element, *length);
    }

    if (type.is_simd()) {
        const std::optional<uint64_t> length = type.bound_simd_length();
        if (!length.has_value()) {
            return nullptr;
        }

        llvm::Type *element = llvm_type_of(type.simd_element(), context, layout, aggregate);
        if (element == nullptr) {
            return nullptr;
        }

        return llvm::FixedVectorType::get(element, static_cast<unsigned>(*length));
    }

    return nullptr;
}

bool fill_aggregate_llvm_type(
    llvm::StructType *st,
    const AST::ComplexType &type,
    llvm::LLVMContext &context,
    const llvm::DataLayout &layout,
    const std::function<llvm::Type *(const AST::ValueType &)> &lower
)
{
    if (st == nullptr) {
        return false;
    }

    if (!st->isOpaque()) {
        return true;
    }

    if (type.is_enum_kind() && type.has_payload_case()) {
        llvm::Type *tag_ty = lowered_field(lower, type.get_property_type(AST::k_enum_tag_index));
        if (tag_ty == nullptr) {
            return false;
        }

        llvm::Type *ptr_ty = llvm::PointerType::get(context, 0);
        const uint64_t ptr_size = layout.getTypeAllocSize(ptr_ty);
        const uint64_t ptr_align = layout.getABITypeAlign(ptr_ty).value();

        uint64_t max_size = 0;
        uint64_t max_align = 1;

        for (const AST::ComplexType::EnumCase &entry : type.enum_cases()) {
            if (!entry.has_payload()) {
                continue;
            }

            if (AST::enum_payload_is_heap(entry)) {
                max_size = std::max<uint64_t>(max_size, ptr_size);
                max_align = std::max<uint64_t>(max_align, ptr_align);
                continue;
            }

            std::vector<llvm::Type *> field_types;
            field_types.reserve(entry.payload_field_count);

            for (size_t i = 0; i < entry.payload_field_count; i++) {
                llvm::Type *field = lowered_field(
                    lower, type.get_property_type(entry.first_payload_property + i));
                if (field == nullptr) {
                    return false;
                }

                field_types.push_back(field);
            }

            llvm::StructType *case_ty = llvm::StructType::get(context, field_types);
            max_size = std::max<uint64_t>(max_size, layout.getTypeAllocSize(case_ty));
            max_align = std::max<uint64_t>(
                max_align, layout.getStructLayout(case_ty)->getAlignment().value());
        }

        if (max_size == 0) {
            st->setBody(std::vector<llvm::Type *>{ tag_ty });
            return true;
        }

        llvm::Type *unit = alignment_unit_type(context, max_align);
        if (unit == nullptr) {
            return false;
        }

        const uint64_t storage_size = llvm::alignTo(max_size, max_align);
        st->setBody({ tag_ty, llvm::ArrayType::get(unit, storage_size / max_align) });
        return true;
    }

    std::vector<llvm::Type *> fields;
    fields.reserve(type.property_count());

    for (size_t i = 0; i < type.property_count(); i++) {
        llvm::Type *field = lowered_field(lower, type.get_property_type(i));
        if (field == nullptr) {
            return false;
        }

        fields.push_back(field);
    }

    st->setBody(fields);
    return true;
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

    auto throwing_lower = [&](const AST::ValueType &field) -> llvm::Type * {
        llvm::Type *ty = lower(field);
        if (ty != nullptr) {
            return ty;
        }

        assert(false);
        throw ctx.error(fmt::format(
            "Unknown type for a field in struct '{}'.",
            type.name.value_or("<anonymous>")
        ));
    };

    if (!fill_aggregate_llvm_type(body_of().llvm_struct, type, context, layout, throwing_lower)) {
        llvm_unreachable("aggregate LLVM body failed after a throwing lower");
    }

    // a payload enum overlays its cases in one storage region. the AST properties stay one field
    // per payload slot - that is what classify_copy and the per-case drop still fold over - and
    // only this LLVM type is packed. a `[N x i8]` *property* would fold to t_bytes
    if (type.is_enum_kind() && type.has_payload_case()) {
        std::vector<uint64_t> relative(type.property_count(), 0);

        Structure &packed = body_of();
        packed.packed_payload = true;
        packed.property_byte_offset.assign(type.property_count(), 0);
        packed.property_in_heap.assign(type.property_count(), 0);
        packed.enum_heap_cell.assign(type.enum_cases().size(), nullptr);
        packed.property_case_ordinal.assign(type.property_count(), ~uint32_t{0});

        for (const AST::ComplexType::EnumCase &entry : type.enum_cases()) {
            if (!entry.has_payload()) {
                continue;
            }

            for (size_t i = 0; i < entry.payload_field_count; i++) {
                packed.property_case_ordinal[entry.first_payload_property + i] =
                    static_cast<uint32_t>(entry.ordinal);
            }

            // an indirect case's cell contains this enum, which is still opaque. measuring it
            // now is a 0-size layout and a GEP offset that is not the overlay. the overlay
            // contribution is one pointer; the cell is built after this enum has a body
            if (AST::enum_payload_is_heap(entry)) {
                for (size_t i = 0; i < entry.payload_field_count; i++) {
                    packed.property_in_heap[entry.first_payload_property + i] = 1;
                }

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
        }

        auto finish_heap_cells = [&]() {
            // the enum has a body now, so a recursive payload can be measured. identified rather
            // than a literal: a literal of still-opaque elements uniqued as size 0
            for (const AST::ComplexType::EnumCase &entry : type.enum_cases()) {
                if (!AST::enum_payload_is_heap(entry)) {
                    continue;
                }

                std::vector<llvm::Type *> field_types;
                field_types.reserve(entry.payload_field_count);

                for (size_t i = 0; i < entry.payload_field_count; i++) {
                    const size_t prop_i = entry.first_payload_property + i;
                    field_types.push_back(require_lowered(
                        lower, type.get_property_type(prop_i), type, prop_i, ctx));
                }

                llvm::StructType *cell_ty = llvm::StructType::create(
                    context,
                    field_types,
                    "eco.enum.cell." + type.name.value_or("anon") + "." + std::to_string(entry.ordinal));
                packed.enum_heap_cell[entry.ordinal] = cell_ty;

                const llvm::StructLayout *cell_layout = layout.getStructLayout(cell_ty);

                for (size_t i = 0; i < entry.payload_field_count; i++) {
                    const size_t prop_i = entry.first_payload_property + i;
                    packed.property_byte_offset[prop_i] = cell_layout->getElementOffset(i);
                }
            }
        };

        // a payload of only zero-sized fields still overlays: the AST has slots, the LLVM
        // type is just the tag, and the offset table names the one-past-the-tag address a
        // zero-sized field lives at. skipping the table here made gep_property CreateStructGEP
        // a missing LLVM field
        if (packed.llvm_struct->getNumElements() == 1) {
            const uint64_t payload_start = layout.getTypeAllocSize(packed.llvm_struct);
            packed.packed_payload_offset = payload_start;

            for (size_t i = 1; i < type.property_count(); i++) {
                if (!packed.property_lives_in_heap(i)) {
                    packed.property_byte_offset[i] = payload_start + relative[i];
                }
            }

            finish_heap_cells();
            return;
        }

        const uint64_t payload_start = layout.getStructLayout(packed.llvm_struct)->getElementOffset(1);
        packed.packed_payload_offset = payload_start;

        for (size_t i = 1; i < type.property_count(); i++) {
            if (!packed.property_lives_in_heap(i)) {
                packed.property_byte_offset[i] = payload_start + relative[i];
            }
        }

        finish_heap_cells();
        return;
    }

    Structure &body = body_of();
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
