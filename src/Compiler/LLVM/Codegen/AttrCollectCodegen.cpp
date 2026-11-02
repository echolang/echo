#include "Compiler/LLVM/Codegen/AttrCollectCodegen.h"

#include "AST/ASTAttrCollect.h"
#include "AST/ASTAttrSchema.h"
#include "AST/ASTAttributeValue.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTValueType.h"
#include "AST/AttributeNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

llvm::Constant *callable_field(
    Compiler::LLVM::CodegenContext &ctx,
    AST::FunctionDeclNode *fn,
    bool as_c
)
{
    llvm::Function *adapt = ctx.types->ensure_function_adapt(
        fn,
        as_c
            ? Compiler::LLVM::TypeLowering::FunctionCallingShape::t_c
            : Compiler::LLVM::TypeLowering::FunctionCallingShape::t_echo);

    if (as_c) {
        return adapt;
    }

    llvm::StructType *callable = ctx.types->callable_llvm_type();
    llvm::Constant *env = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(*ctx.llvm_context, 0));
    return llvm::ConstantStruct::get(callable, { adapt, env });
}

llvm::Constant *type_id_field(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::ValueType &type,
    const AST::ValueType &id_type
)
{
    llvm::GlobalVariable *identity = ctx.types->get_or_create_type_identity(
        type, *ctx.current_cmp_unit);
    llvm::Type *agg = ctx.types->get_llvm_type(id_type, *ctx.current_cmp_unit);
    auto *struct_ty = llvm::cast<llvm::StructType>(agg);

    const AST::ComplexType::Property *desc = ctx.core_types().property(
        AST::CoreTypeKind::t_type_id, "desc");
    if (desc == nullptr) {
        throw ctx.error(fmt::format(
            "'{}' has no property '$desc'",
            ctx.core_types().spelling(AST::CoreTypeKind::t_type_id)));
    }
    const unsigned index = static_cast<unsigned>(desc->index);

    std::vector<llvm::Constant *> fields(struct_ty->getNumElements());
    for (unsigned i = 0; i < struct_ty->getNumElements(); i++) {
        fields[i] = llvm::Constant::getNullValue(struct_ty->getElementType(i));
    }
    fields[index] = identity;
    return llvm::ConstantStruct::get(struct_ty, fields);
}

AST::AttrSliceLayout require_slice_layout(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::ValueType &field
)
{
    auto layout = AST::attr_slice_layout(field);
    if (!layout.has_value() || layout->data == nullptr || layout->len == nullptr) {
        throw ctx.error(fmt::format(
            "attr::of needs slice '$data' and '$len' on '{}'",
            field.get_type_desciption()));
    }
    return *layout;
}

llvm::Constant *payload_constant(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttributeValue &value,
    const AST::ValueType &field,
    const std::string &stem,
    unsigned &serial
);

llvm::Constant *list_as_slice(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttributeValue &value,
    const AST::ValueType &field,
    const std::string &stem,
    unsigned &serial
)
{
    const AST::AttrSliceLayout layout = require_slice_layout(ctx, field);
    const AST::ComplexType::Property *data = layout.data;
    const AST::ComplexType::Property *len = layout.len;
    llvm::Type *slice_ty = ctx.types->get_llvm_type(field, *ctx.current_cmp_unit);
    auto *slice_struct = llvm::cast<llvm::StructType>(slice_ty);

    std::vector<llvm::Constant *> fields(slice_struct->getNumElements());
    for (unsigned i = 0; i < slice_struct->getNumElements(); i++) {
        fields[i] = llvm::Constant::getNullValue(slice_struct->getElementType(i));
    }

    const unsigned data_index = static_cast<unsigned>(data->index);
    const unsigned len_index = static_cast<unsigned>(len->index);
    llvm::Type *len_ty = slice_struct->getElementType(len_index);

    if (value.items.empty()) {
        fields[len_index] = llvm::ConstantInt::get(len_ty, 0);
        return llvm::ConstantStruct::get(slice_struct, fields);
    }

    const AST::ValueType &element = data->type.pointee();
    llvm::Type *elem_ty = ctx.types->get_llvm_type(element, *ctx.current_cmp_unit);
    std::vector<llvm::Constant *> elems;
    elems.reserve(value.items.size());
    for (const AST::AttributeValue &item : value.items) {
        elems.push_back(payload_constant(ctx, item, element, stem, serial));
    }

    llvm::ArrayType *array_ty = llvm::ArrayType::get(elem_ty, elems.size());
    llvm::Constant *array = llvm::ConstantArray::get(array_ty, elems);
    auto *global = new llvm::GlobalVariable(
        *ctx.current_module(),
        array_ty,
        true,
        llvm::GlobalValue::PrivateLinkage,
        array,
        stem + ".p" + std::to_string(serial++));
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    llvm::Constant *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx.llvm_context), 0);
    fields[data_index] = llvm::ConstantExpr::getInBoundsGetElementPtr(
        array_ty, global, llvm::ArrayRef<llvm::Constant *>{ zero, zero });
    fields[len_index] = llvm::ConstantInt::get(len_ty, elems.size());
    return llvm::ConstantStruct::get(slice_struct, fields);
}

llvm::Constant *list_as_inline(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttributeValue &value,
    const AST::ValueType &field,
    const std::string &stem,
    unsigned &serial
)
{
    const AST::ValueType &element = field.array_element();
    llvm::Type *array_ty = ctx.types->get_llvm_type(field, *ctx.current_cmp_unit);
    std::vector<llvm::Constant *> elems;
    elems.reserve(value.items.size());
    for (const AST::AttributeValue &item : value.items) {
        elems.push_back(payload_constant(ctx, item, element, stem, serial));
    }
    return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(array_ty), elems);
}

llvm::Constant *record_as_struct(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttributeValue &value,
    const AST::ValueType &field,
    const std::string &stem,
    unsigned &serial
)
{
    llvm::Type *ty = ctx.types->get_llvm_type(field, *ctx.current_cmp_unit);
    auto *struct_ty = llvm::cast<llvm::StructType>(ty);
    const AST::ComplexType *ct = field.get_complex_type();

    std::vector<llvm::Constant *> fields(struct_ty->getNumElements());
    for (unsigned i = 0; i < struct_ty->getNumElements(); i++) {
        fields[i] = llvm::Constant::getNullValue(struct_ty->getElementType(i));
    }

    for (size_t i = 0; i < ct->property_count(); i++) {
        const AST::ComplexType::Property &property = ct->get_property(i);
        const AST::AttributeField *named = value.find_field(property.name);
        if (named == nullptr) {
            continue;
        }
        fields[static_cast<unsigned>(property.index)] =
            payload_constant(ctx, named->value, property.type, stem, serial);
    }

    return llvm::ConstantStruct::get(struct_ty, fields);
}

llvm::Constant *payload_constant(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttributeValue &value,
    const AST::ValueType &field,
    const std::string &stem,
    unsigned &serial
)
{
    if (field.is_integer_type() && value.kind == AST::AttributeValueKind::t_int) {
        llvm::Type *ty = ctx.types->get_llvm_type(field, *ctx.current_cmp_unit);
        return llvm::ConstantInt::get(ty, static_cast<uint64_t>(value.integer), field.is_signed_integer());
    }

    if (field.is_boolean_type() && value.kind == AST::AttributeValueKind::t_bool) {
        return llvm::ConstantInt::get(ctx.types->get_llvm_type(field, *ctx.current_cmp_unit), value.boolean);
    }

    if (field.is_floating_type() && value.kind == AST::AttributeValueKind::t_float) {
        return llvm::ConstantFP::get(ctx.types->get_llvm_type(field, *ctx.current_cmp_unit), value.number);
    }

    if (ctx.core_types().is_string(field)
        && (value.kind == AST::AttributeValueKind::t_string || value.kind == AST::AttributeValueKind::t_name)) {
        return ctx.string_constant(value.text, field);
    }

    if (value.kind == AST::AttributeValueKind::t_list && AST::attr_slice_layout(field).has_value()) {
        return list_as_slice(ctx, value, field, stem, serial);
    }

    if (value.kind == AST::AttributeValueKind::t_list && field.is_inline_array()) {
        return list_as_inline(ctx, value, field, stem, serial);
    }

    if (value.kind == AST::AttributeValueKind::t_record && field.is_struct() && field.has_property_layout()) {
        return record_as_struct(ctx, value, field, stem, serial);
    }

    throw ctx.error(fmt::format(
        "attr::of cannot lower a '{}' payload into '{}'",
        AST::attribute_value_noun(value.kind),
        field.get_type_desciption()));
}

llvm::Constant *row_constant(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttrPlan &plan,
    const AST::AttrRow &row,
    unsigned &serial
)
{
    llvm::Type *row_ty = ctx.types->get_llvm_type(plan.schema.row_type, *ctx.current_cmp_unit);
    auto *struct_ty = llvm::cast<llvm::StructType>(row_ty);
    const std::string stem = AST::attr_table_symbol(plan);

    std::vector<llvm::Constant *> fields(struct_ty->getNumElements());
    for (unsigned i = 0; i < struct_ty->getNumElements(); i++) {
        fields[i] = llvm::Constant::getNullValue(struct_ty->getElementType(i));
    }

    for (const AST::AttrPayloadSlot &slot : row.payload) {
        if (slot.value == nullptr) {
            continue;
        }

        fields[static_cast<unsigned>(slot.index)] =
            payload_constant(ctx, *slot.value, slot.type, stem, serial);
    }

    const unsigned target = static_cast<unsigned>(plan.schema.target_index);

    if (plan.schema.target_kind == AST::AttrTargetKind::t_callable
        || plan.schema.target_kind == AST::AttrTargetKind::t_c_function) {
        fields[target] = callable_field(
            ctx,
            row.match.function,
            plan.schema.target_kind == AST::AttrTargetKind::t_c_function);
    } else if (plan.schema.target_kind == AST::AttrTargetKind::t_type_id && row.match.type != nullptr) {
        fields[target] = type_id_field(
            ctx,
            row.match.type->value_type(),
            plan.schema.target_type);
    }

    return llvm::ConstantStruct::get(struct_ty, fields);
}

llvm::Constant *slice_constant(Compiler::LLVM::CodegenContext &ctx, const AST::AttrPlan &plan)
{
    llvm::Type *row_ty = ctx.types->get_llvm_type(plan.schema.row_type, *ctx.current_cmp_unit);
    llvm::Type *slice_ty = ctx.types->get_llvm_type(plan.table_type, *ctx.current_cmp_unit);
    auto *slice_struct = llvm::cast<llvm::StructType>(slice_ty);

    const AST::AttrSliceLayout layout = require_slice_layout(ctx, plan.table_type);
    const unsigned data_index = static_cast<unsigned>(layout.data->index);
    const unsigned len_index = static_cast<unsigned>(layout.len->index);

    std::vector<llvm::Constant *> fields(slice_struct->getNumElements());
    for (unsigned i = 0; i < slice_struct->getNumElements(); i++) {
        fields[i] = llvm::Constant::getNullValue(slice_struct->getElementType(i));
    }

    llvm::Type *len_ty = slice_struct->getElementType(len_index);

    if (plan.rows.empty()) {
        fields[len_index] = llvm::ConstantInt::get(len_ty, 0);
        return llvm::ConstantStruct::get(slice_struct, fields);
    }

    std::vector<llvm::Constant *> rows;
    rows.reserve(plan.rows.size());
    unsigned serial = 0;
    for (const AST::AttrRow &row : plan.rows) {
        rows.push_back(row_constant(ctx, plan, row, serial));
    }

    llvm::ArrayType *array_ty = llvm::ArrayType::get(row_ty, rows.size());
    llvm::Constant *array = llvm::ConstantArray::get(array_ty, rows);

    auto *global = new llvm::GlobalVariable(
        *ctx.current_module(),
        array_ty,
        true,
        llvm::GlobalValue::PrivateLinkage,
        array,
        AST::attr_table_symbol(plan) + ".table");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    llvm::Constant *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx.llvm_context), 0);
    fields[data_index] = llvm::ConstantExpr::getInBoundsGetElementPtr(
        array_ty, global, llvm::ArrayRef<llvm::Constant *>{ zero, zero });
    fields[len_index] = llvm::ConstantInt::get(len_ty, rows.size());
    return llvm::ConstantStruct::get(slice_struct, fields);
}

llvm::GlobalVariable *slice_global(
    Compiler::LLVM::CodegenContext &ctx,
    const AST::AttrPlan &plan,
    bool define
)
{
    const std::string name = AST::attr_table_symbol(plan);
    llvm::Type *slice_ty = ctx.types->get_llvm_type(plan.table_type, *ctx.current_cmp_unit);
    llvm::Module *module = ctx.current_module();

    llvm::GlobalVariable *gv = module->getGlobalVariable(name, true);
    if (gv == nullptr) {
        gv = new llvm::GlobalVariable(
            *module,
            slice_ty,
            true,
            llvm::GlobalValue::ExternalLinkage,
            define ? slice_constant(ctx, plan) : nullptr,
            name);
        return gv;
    }

    if (define && gv->isDeclaration()) {
        gv->setInitializer(slice_constant(ctx, plan));
        gv->setConstant(true);
        gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }

    return gv;
}

}

void Compiler::LLVM::emit_attr_tables(CodegenContext &ctx)
{
    if (ctx.attr_plans == nullptr) {
        return;
    }

    std::vector<const AST::AttrPlan *> plans;
    for (const auto &entry : ctx.attr_plans->entries) {
        if (entry.second.status == AST::AttrPlanStatus::t_ready) {
            plans.push_back(&entry.second);
        }
    }
    std::sort(plans.begin(), plans.end(), [](const AST::AttrPlan *a, const AST::AttrPlan *b) {
        return AST::attr_table_symbol(*a) < AST::attr_table_symbol(*b);
    });
    for (const AST::AttrPlan *plan : plans) {
        slice_global(ctx, *plan, true);
    }
}

void Compiler::LLVM::gen_attr_of_builtin(CodegenContext &ctx, AST::FunctionCallExprNode &node)
{
    const AST::AttrPlan *plan = ctx.attr_plans != nullptr
        ? AST::attr_plan_lookup(node, *ctx.attr_plans)
        : nullptr;
    if (plan == nullptr || plan->status != AST::AttrPlanStatus::t_ready) {
        throw ctx.error(fmt::format(
            "compiler bug: attr::of reached codegen without a ready table {}",
            ctx.function_context()));
    }

    llvm::GlobalVariable *gv = slice_global(ctx, *plan, false);
    llvm::Type *slice_ty = ctx.types->get_llvm_type(plan->table_type, *ctx.current_cmp_unit);
    ctx.push_scalar(ctx.builder->CreateLoad(slice_ty, gv, "attr.table"));
}
