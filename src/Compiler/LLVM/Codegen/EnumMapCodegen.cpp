#include "Compiler/LLVM/Codegen/EnumMapCodegen.h"

#include "AST/ASTEnumMap.h"
#include "AST/ASTMangler.h"
#include "AST/ASTValueType.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>

#include <fmt/core.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace
{
constexpr uint64_t k_max_lut_span = 65536;

llvm::GlobalVariable *get_or_create_lut(
    Compiler::LLVM::CodegenContext &ctx,
    const std::string &symbol,
    llvm::ArrayType *type,
    llvm::Constant *init
)
{
    if (auto *existing = ctx.current_module()->getGlobalVariable(symbol, true)) {
        return existing;
    }

    auto *global = new llvm::GlobalVariable(
        *ctx.current_module(),
        type,
        /*isConstant=*/true,
        llvm::GlobalValue::LinkOnceODRLinkage,
        init,
        symbol);

    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(ctx.layout().getABITypeAlign(type));
    return global;
}

llvm::Value *load_arg(Compiler::LLVM::CodegenContext &ctx, AST::VarDeclNode *decl, const char *name)
{
    auto found = ctx.var_map.find(decl);
    if (found == ctx.var_map.end()) {
        throw ctx.error(fmt::format(
            "parameter '{}' has no allocation in an enum map body {}",
            decl != nullptr ? decl->name() : "?", ctx.function_context()));
    }

    llvm::Type *stored = found->second->getAllocatedType();
    return ctx.builder->CreateLoad(stored, found->second, name);
}

llvm::Value *load_enum_tag(
    Compiler::LLVM::CodegenContext &ctx,
    llvm::Value *enum_address,
    const AST::ValueType &enum_type
)
{
    const AST::ComplexType *ct = enum_type.get_complex_type();
    Compiler::LLVM::LValue place { enum_address, enum_type, Compiler::LLVM::Provenance::t_typed };

    return ctx.lvalues->gen_load(
        ctx.lvalues->property_place(
            ctx.lvalues->structure_of(ct, enum_type),
            place,
            AST::k_enum_tag_index,
            ct->get_property_type(AST::k_enum_tag_index),
            "enum.tag_ptr"),
        "enum.tag");
}

struct FoldedMap
{
    std::vector<uint64_t> by_ordinal;
    // empty means every ordinal is present (closed `from`). 1 = this source case was mapped
    std::vector<uint8_t> present;
    std::vector<int64_t> discriminants;
    bool values_signed = true;
};

bool mapped(const FoldedMap &folded, size_t i)
{
    return folded.present.empty() || folded.present[i] != 0;
}

std::optional<FoldedMap> fold_named_map(const AST::EnumMap &map, const AST::ComplexType &ct)
{
    FoldedMap out;
    out.by_ordinal.assign(ct.enum_cases().size(), 0);
    out.present.assign(ct.enum_cases().size(), 0);
    out.discriminants.reserve(ct.enum_cases().size());
    const AST::ValueType key_type = AST::enum_map_key_type(map.value_type);
    out.values_signed = AST::get_integer_size(key_type.get_primitive_type()).is_signed;

    for (const AST::ComplexType::EnumCase &entry : ct.enum_cases()) {
        out.discriminants.push_back(entry.discriminant);
    }

    for (const AST::EnumMap::Association &assoc : map.associations) {
        if (!assoc.folded_bits.has_value() || assoc.case_ordinal >= out.by_ordinal.size()) {
            return std::nullopt;
        }

        out.by_ordinal[assoc.case_ordinal] = *assoc.folded_bits;
        out.present[assoc.case_ordinal] = 1;
    }

    if (map.total) {
        out.present.clear();
    }

    return out;
}

FoldedMap closed_from_map(const AST::ComplexType &ct)
{
    FoldedMap out;
    out.by_ordinal.reserve(ct.enum_cases().size());
    out.discriminants.reserve(ct.enum_cases().size());
    if (ct.enum_backing.has_value() && ct.enum_backing->is_integer_type()) {
        out.values_signed = AST::get_integer_size(ct.enum_backing->get_primitive_type()).is_signed;
    }

    for (const AST::ComplexType::EnumCase &entry : ct.enum_cases()) {
        out.by_ordinal.push_back(static_cast<uint64_t>(entry.discriminant));
        out.discriminants.push_back(entry.discriminant);
    }

    return out;
}

bool span_of(
    const std::vector<uint64_t> &values,
    bool is_signed,
    int64_t &min_out,
    uint64_t &span_out
)
{
    if (values.empty()) {
        return false;
    }

    if (is_signed) {
        int64_t min = static_cast<int64_t>(values[0]);
        int64_t max = min;
        for (uint64_t bits : values) {
            const int64_t value = static_cast<int64_t>(bits);
            min = std::min(min, value);
            max = std::max(max, value);
        }

        const uint64_t diff = static_cast<uint64_t>(max) - static_cast<uint64_t>(min);
        if (diff == UINT64_MAX) {
            return false;
        }

        const uint64_t span = diff + 1;
        if (span > k_max_lut_span) {
            return false;
        }

        min_out = min;
        span_out = span;
        return true;
    }

    uint64_t min = values[0];
    uint64_t max = min;
    for (uint64_t value : values) {
        min = std::min(min, value);
        max = std::max(max, value);
    }

    const uint64_t diff = max - min;
    if (diff == UINT64_MAX) {
        return false;
    }

    const uint64_t span = diff + 1;
    if (span > k_max_lut_span) {
        return false;
    }

    min_out = static_cast<int64_t>(min);
    span_out = span;
    return true;
}

uint64_t slot_of(uint64_t bits, int64_t min, bool is_signed)
{
    if (is_signed) {
        return static_cast<uint64_t>(static_cast<int64_t>(bits) - min);
    }

    return bits - static_cast<uint64_t>(min);
}

llvm::Constant *int_constant(llvm::Type *type, uint64_t bits, bool is_signed)
{
    if (is_signed) {
        return llvm::ConstantInt::getSigned(
            llvm::cast<llvm::IntegerType>(type), static_cast<int64_t>(bits));
    }

    return llvm::ConstantInt::get(type, bits);
}

llvm::GlobalVariable *emit_i32_table(
    Compiler::LLVM::CodegenContext &ctx,
    const std::string &symbol,
    const std::vector<int32_t> &values
)
{
    llvm::Type *i32 = llvm::Type::getInt32Ty(*ctx.llvm_context);
    auto *array_ty = llvm::ArrayType::get(i32, values.size());

    std::vector<llvm::Constant *> elems;
    elems.reserve(values.size());
    for (int32_t value : values) {
        elems.push_back(llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(i32), value));
    }

    return get_or_create_lut(ctx, symbol, array_ty, llvm::ConstantArray::get(array_ty, elems));
}

llvm::Value *gep_load(
    Compiler::LLVM::CodegenContext &ctx,
    llvm::GlobalVariable *table,
    llvm::ArrayType *array_ty,
    llvm::Type *elem_ty,
    llvm::Value *index,
    const char *name
)
{
    llvm::Value *zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvm_context), 0);
    llvm::Value *idx64 = ctx.builder->CreateSExtOrTrunc(index, llvm::Type::getInt64Ty(*ctx.llvm_context));
    llvm::Value *ptr = ctx.builder->CreateInBoundsGEP(array_ty, table, { zero, idx64 }, name);
    return ctx.builder->CreateLoad(elem_ty, ptr, (std::string(name) + ".val").c_str());
}

llvm::Value *extend_to_i64(
    Compiler::LLVM::CodegenContext &ctx,
    llvm::Value *value,
    bool is_signed,
    const char *name
)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*ctx.llvm_context);
    if (is_signed) {
        return ctx.builder->CreateSExtOrTrunc(value, i64, name);
    }

    return ctx.builder->CreateZExtOrTrunc(value, i64, name);
}

llvm::Constant *i64_offset(Compiler::LLVM::CodegenContext &ctx, int64_t min, bool is_signed)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*ctx.llvm_context);
    if (is_signed) {
        return llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(i64), min);
    }

    return llvm::ConstantInt::get(i64, static_cast<uint64_t>(min));
}

llvm::Value *assemble_enum(
    Compiler::LLVM::CodegenContext &ctx,
    llvm::Value *tag,
    const AST::ValueType &enum_type
)
{
    llvm::Type *enum_ty = ctx.types->get_llvm_type(enum_type, *ctx.current_cmp_unit);
    llvm::Value *value = llvm::UndefValue::get(enum_ty);
    return ctx.builder->CreateInsertValue(value, tag, { AST::k_enum_tag_index });
}

llvm::Value *assemble_present_optional(
    Compiler::LLVM::CodegenContext &ctx,
    llvm::Value *payload,
    const AST::ValueType &optional
)
{
    llvm::Type *opt_ty = ctx.types->get_llvm_type(optional, *ctx.current_cmp_unit);
    auto *opt_struct = llvm::cast<llvm::StructType>(opt_ty);
    llvm::Type *has_ty = opt_struct->getElementType(AST::k_optional_has_index);
    llvm::Value *value = llvm::UndefValue::get(opt_ty);
    value = ctx.builder->CreateInsertValue(
        value, llvm::ConstantInt::get(has_ty, 1), { AST::k_optional_has_index });
    return ctx.builder->CreateInsertValue(
        value, payload, { AST::k_optional_value_index });
}

llvm::Value *assemble_range_value(
    Compiler::LLVM::CodegenContext &ctx,
    llvm::Value *loaded,
    const AST::ValueType &range
)
{
    if (range.is_enum()) {
        return assemble_enum(ctx, loaded, range);
    }

    return loaded;
}

bool emit_forward(
    Compiler::LLVM::CodegenContext &ctx,
    AST::FunctionDeclNode &node,
    const FoldedMap &folded
)
{
    const AST::ValueType result = node.get_return_type();
    const AST::ValueType range = result.is_wrapped_optional()
        ? result.optional_payload()
        : result;
    const AST::ValueType key_type = AST::enum_map_key_type(range);
    llvm::Type *elem_ty = ctx.types->get_llvm_type(key_type, *ctx.current_cmp_unit);
    const bool is_signed = folded.values_signed;

    const AST::ValueType receiver = node.args[0]->type();
    const AST::ValueType enum_type = receiver.is_pointer() ? receiver.pointee() : receiver;
    const AST::ValueType tag_type = enum_type.get_complex_type()->get_property_type(AST::k_enum_tag_index);
    const bool tags_signed = AST::get_integer_size(tag_type.get_primitive_type()).is_signed;

    std::vector<uint64_t> disc_bits;
    disc_bits.reserve(folded.discriminants.size());
    for (int64_t disc : folded.discriminants) {
        disc_bits.push_back(static_cast<uint64_t>(disc));
    }

    int64_t disc_min = 0;
    uint64_t disc_span = 0;
    if (!span_of(disc_bits, tags_signed, disc_min, disc_span)) {
        return false;
    }

    auto *array_ty = llvm::ArrayType::get(elem_ty, folded.by_ordinal.size());
    std::vector<llvm::Constant *> elems;
    elems.reserve(folded.by_ordinal.size());
    for (uint64_t bits : folded.by_ordinal) {
        elems.push_back(int_constant(elem_ty, bits, is_signed));
    }

    llvm::GlobalVariable *table = get_or_create_lut(
        ctx,
        AST::mangle_function_name(&node) + ".lut.fwd",
        array_ty,
        llvm::ConstantArray::get(array_ty, elems));

    std::vector<int32_t> rev(disc_span, -1);
    for (size_t i = 0; i < folded.discriminants.size(); i++) {
        rev[slot_of(static_cast<uint64_t>(folded.discriminants[i]), disc_min, tags_signed)] =
            static_cast<int32_t>(i);
    }

    llvm::Type *i32 = llvm::Type::getInt32Ty(*ctx.llvm_context);
    llvm::GlobalVariable *ord_table = emit_i32_table(
        ctx, AST::mangle_function_name(&node) + ".lut.ord", rev);

    llvm::Value *this_addr = load_arg(ctx, node.args[0], "this.ptr");
    llvm::Value *tag = load_enum_tag(ctx, this_addr, enum_type);
    llvm::Value *tag64 = extend_to_i64(ctx, tag, tags_signed, "enum.tag64");
    llvm::Value *idx = ctx.builder->CreateSub(
        tag64, i64_offset(ctx, disc_min, tags_signed), "enum.idx");
    llvm::Value *ordinal = gep_load(
        ctx,
        ord_table,
        llvm::ArrayType::get(i32, disc_span),
        i32,
        idx,
        "enum.ord");

    llvm::Value *loaded = gep_load(ctx, table, array_ty, elem_ty, ordinal, "map.fwd");

    if (result.is_wrapped_optional()) {
        std::vector<int32_t> pres(folded.by_ordinal.size(), 0);
        for (size_t i = 0; i < folded.by_ordinal.size(); i++) {
            if (mapped(folded, i)) {
                pres[i] = 1;
            }
        }

        llvm::GlobalVariable *pres_table = emit_i32_table(
            ctx, AST::mangle_function_name(&node) + ".lut.pres", pres);
        llvm::Value *flag = gep_load(
            ctx,
            pres_table,
            llvm::ArrayType::get(i32, pres.size()),
            i32,
            ordinal,
            "map.pres");

        llvm::Function *function = ctx.builder->GetInsertBlock()->getParent();
        auto *found = llvm::BasicBlock::Create(*ctx.llvm_context, "map.found", function);
        auto *miss = llvm::BasicBlock::Create(*ctx.llvm_context, "map.miss", function);
        llvm::Value *has = ctx.builder->CreateICmpNE(
            flag, llvm::ConstantInt::get(i32, 0), "map.has");
        ctx.builder->CreateCondBr(has, found, miss);

        ctx.set_insert_point(found);
        ctx.types->emit_returned_value(
            assemble_present_optional(ctx, assemble_range_value(ctx, loaded, range), result));

        ctx.set_insert_point(miss);
        ctx.types->emit_returned_value(
            ctx.types->gen_absent(result, *ctx.current_cmp_unit));
        return true;
    }

    ctx.types->emit_returned_value(assemble_range_value(ctx, loaded, range));
    return true;
}

bool emit_reverse(
    Compiler::LLVM::CodegenContext &ctx,
    AST::FunctionDeclNode &node,
    const FoldedMap &folded,
    const std::string &symbol_prefix
)
{
    int64_t min = 0;
    uint64_t span = 0;
    std::vector<uint64_t> mapped_values;
    mapped_values.reserve(folded.by_ordinal.size());
    for (size_t i = 0; i < folded.by_ordinal.size(); i++) {
        if (mapped(folded, i)) {
            mapped_values.push_back(folded.by_ordinal[i]);
        }
    }

    if (!span_of(mapped_values, folded.values_signed, min, span)) {
        return false;
    }

    std::vector<int32_t> rev(span, -1);
    for (size_t i = 0; i < folded.by_ordinal.size(); i++) {
        if (!mapped(folded, i)) {
            continue;
        }

        rev[slot_of(folded.by_ordinal[i], min, folded.values_signed)] = static_cast<int32_t>(i);
    }

    llvm::Type *i32 = llvm::Type::getInt32Ty(*ctx.llvm_context);
    llvm::GlobalVariable *rev_table = emit_i32_table(ctx, symbol_prefix + ".lut.rev", rev);

    const AST::ValueType optional = node.get_return_type();
    const AST::ValueType enum_type = optional.optional_payload();
    const AST::ValueType tag_type = enum_type.get_complex_type()->get_property_type(AST::k_enum_tag_index);
    llvm::Type *tag_ty = ctx.types->get_llvm_type(tag_type, *ctx.current_cmp_unit);
    const bool tags_signed = AST::get_integer_size(tag_type.get_primitive_type()).is_signed;

    auto *disc_array_ty = llvm::ArrayType::get(tag_ty, folded.discriminants.size());
    std::vector<llvm::Constant *> disc_elems;
    disc_elems.reserve(folded.discriminants.size());
    for (int64_t disc : folded.discriminants) {
        disc_elems.push_back(int_constant(tag_ty, static_cast<uint64_t>(disc), tags_signed));
    }

    llvm::GlobalVariable *disc_table = get_or_create_lut(
        ctx,
        symbol_prefix + ".lut.disc",
        disc_array_ty,
        llvm::ConstantArray::get(disc_array_ty, disc_elems));

    llvm::Value *raw = nullptr;
    const AST::ValueType raw_type = node.args[0]->type();
    if (raw_type.is_enum()) {
        auto slot = ctx.var_map.find(node.args[0]);
        if (slot == ctx.var_map.end()) {
            throw ctx.error(fmt::format(
                "parameter '{}' has no allocation in an enum map body {}",
                node.args[0]->name(), ctx.function_context()));
        }

        raw = load_enum_tag(ctx, slot->second, raw_type);
    }
    else {
        raw = load_arg(ctx, node.args[0], "raw");
    }

    llvm::Value *raw64 = extend_to_i64(ctx, raw, folded.values_signed, "raw.i64");
    llvm::Value *idx = ctx.builder->CreateSub(
        raw64, i64_offset(ctx, min, folded.values_signed), "map.idx");

    llvm::Function *function = ctx.builder->GetInsertBlock()->getParent();
    auto *hit = llvm::BasicBlock::Create(*ctx.llvm_context, "map.hit", function);
    auto *found = llvm::BasicBlock::Create(*ctx.llvm_context, "map.found", function);
    auto *miss = llvm::BasicBlock::Create(*ctx.llvm_context, "map.miss", function);

    llvm::Type *i64 = llvm::Type::getInt64Ty(*ctx.llvm_context);
    llvm::Value *in_span = ctx.builder->CreateICmpULT(
        idx, llvm::ConstantInt::get(i64, span), "map.in_span");
    ctx.builder->CreateCondBr(in_span, hit, miss);

    ctx.set_insert_point(hit);
    llvm::Value *ordinal = gep_load(
        ctx, rev_table, llvm::ArrayType::get(i32, span), i32, idx, "map.ord");
    llvm::Value *present = ctx.builder->CreateICmpSGE(
        ordinal, llvm::ConstantInt::get(i32, 0), "map.present");
    ctx.builder->CreateCondBr(present, found, miss);

    ctx.set_insert_point(found);
    llvm::Value *disc = gep_load(ctx, disc_table, disc_array_ty, tag_ty, ordinal, "map.disc");
    ctx.types->emit_returned_value(
        assemble_present_optional(ctx, assemble_enum(ctx, disc, enum_type), optional));

    ctx.set_insert_point(miss);
    ctx.types->emit_returned_value(
        ctx.types->gen_absent(optional, *ctx.current_cmp_unit));

    return true;
}
}

bool Compiler::LLVM::try_gen_enum_lut(CodegenContext &ctx, AST::FunctionDeclNode &node)
{
    const AST::EnumLut lut = AST::enum_lut_of(&node);
    if (lut.kind == AST::EnumLutKind::t_none || node.owner_type == nullptr) {
        return false;
    }

    const AST::ComplexType *ct = node.owner_type->template_or_self();
    const std::string prefix = AST::mangle_function_name(&node);

    if (lut.kind == AST::EnumLutKind::t_forward || lut.kind == AST::EnumLutKind::t_reverse) {
        if (lut.map == nullptr) {
            return false;
        }

        std::optional<FoldedMap> folded = fold_named_map(*lut.map, *ct);
        if (!folded.has_value()) {
            return false;
        }

        if (lut.kind == AST::EnumLutKind::t_forward) {
            return emit_forward(ctx, node, *folded);
        }

        return emit_reverse(ctx, node, *folded, prefix);
    }

    FoldedMap folded = closed_from_map(*ct);
    return emit_reverse(ctx, node, folded, prefix);
}
