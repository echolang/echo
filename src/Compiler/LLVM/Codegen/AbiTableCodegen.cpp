#include "Compiler/LLVM/Codegen/AbiTableCodegen.h"

#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTAbi.h"
#include "AST/ASTConstFold.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTExternInterface.h"
#include "AST/ASTModule.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <vector>

namespace Compiler::LLVM
{

static unsigned header_field(CodegenContext &ctx, const char *name)
{
    const AST::ComplexType::Property *prop = ctx.core_types().property(
        AST::CoreTypeKind::t_abi_header, name);

    if (prop == nullptr) {
        throw ctx.error(fmt::format(
            "'{}' has no property '${}'",
            ctx.core_types().spelling(AST::CoreTypeKind::t_abi_header),
            name));
    }

    return static_cast<unsigned>(prop->index);
}

void AbiCodegen::emit_table(AST::ComplexType &conformer, AST::ComplexType &iface)
{
    const std::string name = AST::abi_table_symbol(iface);

    if (_ctx.current_module()->getGlobalVariable(name, true) != nullptr) {
        return;
    }

    if (!_ctx.core_types().has(AST::CoreTypeKind::t_abi_header)) {
        throw _ctx.error("an ABI table needs the standard library");
    }

    const AST::ValueType header_type = _ctx.core_types().type(AST::CoreTypeKind::t_abi_header);
    llvm::StructType *header_llvm = llvm::cast<llvm::StructType>(
        _ctx.types->get_llvm_type(header_type, *_ctx.current_cmp_unit));

    const auto requirements = AST::extern_interface_requirements(iface);
    const unsigned n = static_cast<unsigned>(requirements.size());
    llvm::Type *ptr_ty = llvm::PointerType::get(*_ctx.llvm_context, 0);
    llvm::ArrayType *slots_ty = llvm::ArrayType::get(ptr_ty, n);
    llvm::StructType *table_ty = llvm::StructType::get(
        *_ctx.llvm_context, { header_llvm, slots_ty });

    const unsigned field_count = header_llvm->getNumElements();
    std::vector<llvm::Constant *> header_fields(field_count);

    for (unsigned i = 0; i < field_count; ++i) {
        header_fields[i] = llvm::Constant::getNullValue(header_llvm->getElementType(i));
    }

    header_fields[header_field(_ctx, "version")] = llvm::ConstantInt::get(
        header_llvm->getElementType(header_field(_ctx, "version")),
        AST::k_abi_version);
    header_fields[header_field(_ctx, "slot_count")] = llvm::ConstantInt::get(
        header_llvm->getElementType(header_field(_ctx, "slot_count")),
        n);
    header_fields[header_field(_ctx, "fingerprint")] = llvm::ConstantInt::get(
        header_llvm->getElementType(header_field(_ctx, "fingerprint")),
        AST::abi_fingerprint(iface));

    llvm::Constant *header = llvm::ConstantStruct::get(header_llvm, header_fields);

    std::vector<llvm::Constant *> slots;
    slots.reserve(n);

    for (AST::FunctionDeclNode *requirement : requirements) {
        AST::ExternConformanceAnswer answer =
            AST::extern_conformance_answer(conformer, *requirement);

        if (answer.decl == nullptr) {
            throw _ctx.error(fmt::format(
                "ABI table for '{}' has no static answering '{}'",
                iface.namespaced_name(),
                requirement->func_name()));
        }

        llvm::Function *adapt = _ctx.types->ensure_function_adapt(
            answer.decl, TypeLowering::FunctionCallingShape::t_c);
        slots.push_back(adapt);
    }

    llvm::Constant *slot_array = llvm::ConstantArray::get(slots_ty, slots);
    llvm::Constant *init = llvm::ConstantStruct::get(table_ty, { header, slot_array });

    auto *gv = new llvm::GlobalVariable(
        *_ctx.current_module(),
        table_ty,
        true,
        llvm::GlobalValue::ExternalLinkage,
        init,
        name);

    if (_ctx.options.codegen.is_native_library()) {
        gv->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    }
}

void AbiCodegen::emit_tables()
{
    AST::Module *module = _ctx.current_cmp_unit->ast_module;

    if (module == nullptr) {
        return;
    }

    for (AST::TypeDeclNode *node : module->nodes.of_type<AST::TypeDeclNode>()) {
        if (node == nullptr || !AST::type_exports_abi_table(*node)) {
            continue;
        }

        AST::ComplexType &conformer = node->complex_type();

        for (const AST::ValueType &iface_type : conformer.extern_conformances()) {
            AST::ComplexType *iface = iface_type.get_complex_type();

            if (iface == nullptr || !iface->is_extern_interface_kind()) {
                continue;
            }

            emit_table(conformer, *iface);
        }
    }
}

const AST::ComplexType *AbiCodegen::subject(AST::FunctionCallExprNode &node, const char *name)
{
    const AST::FunctionDeclNode *decl = node.decl;

    if (decl->instantiation_args.size() != 1) {
        throw _ctx.error(fmt::format(
            "'{}' expects exactly one type argument, got {} {}",
            name, decl->instantiation_args.size(), _ctx.function_context()));
    }

    const AST::ComplexType *ct = decl->instantiation_args[0].get_complex_type();

    if (ct == nullptr || !ct->is_extern_interface_kind()) {
        throw _ctx.error(fmt::format(
            "'{}' needs an extern interface {}", name, _ctx.function_context()));
    }

    return ct;
}

void AbiCodegen::gen_fact(AST::FunctionCallExprNode &node)
{
    const AST::ConstFoldResult folded = AST::const_fold(&node);

    if (folded.result != AST::ConstFoldResult::Result::t_folded) {
        throw _ctx.error(fmt::format(
            "Builtin '{}' did not fold for a settled call: {} {}",
            node.decl->builtin.value(),
            folded.refusal.empty() ? "its type argument is not bound" : folded.refusal,
            _ctx.function_context()));
    }

    llvm::Type *result_type = _ctx.types->get_llvm_type(
        node.decl->get_return_type(), *_ctx.current_cmp_unit);
    _ctx.push_scalar(llvm::ConstantInt::get(result_type, folded.bits));
}

void AbiCodegen::gen_symbol(AST::FunctionCallExprNode &node)
{
    const AST::ComplexType *ct = subject(node, "abi::symbol");
    _ctx.push_scalar(_ctx.string_constant(
        AST::abi_table_symbol(*ct), node.decl->get_return_type()));
}

void AbiCodegen::gen_slot_symbol(AST::FunctionCallExprNode &node)
{
    const AST::ComplexType *ct = subject(node, "abi::slot_symbol");

    if (node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format(
            "'abi::slot_symbol' takes one index {}", _ctx.function_context()));
    }

    const auto requirements = AST::extern_interface_requirements(*ct);
    const AST::ValueType string_type = node.decl->get_return_type();
    std::vector<llvm::Constant *> names;
    names.reserve(requirements.size());

    for (AST::FunctionDeclNode *requirement : requirements) {
        const std::string link = requirement->abi_link_name.value_or(requirement->func_name());
        names.push_back(_ctx.string_constant(link, string_type));
    }

    llvm::Type *elem_ty = _ctx.types->get_llvm_type(string_type, *_ctx.current_cmp_unit);
    llvm::ArrayType *arr_ty = llvm::ArrayType::get(elem_ty, names.size());
    llvm::Constant *arr = llvm::ConstantArray::get(arr_ty, names);

    auto *gv = new llvm::GlobalVariable(
        *_ctx.current_module(),
        arr_ty,
        true,
        llvm::GlobalValue::PrivateLinkage,
        arr,
        "abi.slot_names");

    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *index = _ctx.pop_scalar();

    if (_ctx.options.assertions_enabled()) {
        llvm::Value *n = llvm::ConstantInt::get(index->getType(), names.size());
        llvm::Value *oob = _ctx.builder->CreateICmpUGE(index, n, "abi.slot.oob");
        _ctx.abort->gen_abort_if(
            oob, "fatal error", "abi::slot_symbol index is out of range",
            node.token_function_name);
    }

    llvm::Value *zero = llvm::ConstantInt::get(index->getType(), 0);
    llvm::Value *elem = _ctx.builder->CreateInBoundsGEP(
        arr_ty, gv, { zero, index }, "abi.slot_name");
    _ctx.push_scalar(_ctx.builder->CreateLoad(elem_ty, elem, "abi.slot_name.val"));
}

void AbiCodegen::gen_bind_unchecked(AST::FunctionCallExprNode &node)
{
    const AST::ComplexType *ct = subject(node, "abi::bind_unchecked");

    if (node.arguments.size() != 2 || node.arguments[0] == nullptr || node.arguments[1] == nullptr) {
        throw _ctx.error(fmt::format(
            "'abi::bind_unchecked' takes slots and an image {}", _ctx.function_context()));
    }

    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *slots = _ctx.pop_scalar();

    node.arguments[1]->accept(*_ctx.visitor);
    llvm::Value *image = _ctx.pop_scalar();

    llvm::Type *agg_ty = _ctx.types->get_llvm_type(
        node.decl->get_return_type(), *_ctx.current_cmp_unit);
    llvm::Value *agg = llvm::UndefValue::get(agg_ty);
    llvm::Type *ptr_ty = llvm::PointerType::get(*_ctx.llvm_context, 0);

    size_t slot_i = 0;

    for (size_t i = 0; i < ct->property_count(); ++i) {
        const AST::ComplexType::Property &prop = ct->get_property(i);
        const unsigned idx = static_cast<unsigned>(prop.index);

        if (prop.name == AST::k_extern_image_property) {
            agg = _ctx.builder->CreateInsertValue(agg, image, { idx }, "abi.image");
            continue;
        }

        llvm::Value *off = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(*_ctx.llvm_context), slot_i);
        llvm::Value *addr = _ctx.builder->CreateInBoundsGEP(
            ptr_ty, slots, off, "abi.slot.addr");
        llvm::Value *fn = _ctx.builder->CreateLoad(ptr_ty, addr, "abi.slot");
        agg = _ctx.builder->CreateInsertValue(agg, fn, { idx }, "abi.slot");
        ++slot_i;
    }

    _ctx.push_scalar(agg);
}

void AbiCodegen::gen_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind)
{
    switch (kind) {
        case AST::BuiltinKind::t_abi_version:
        case AST::BuiltinKind::t_abi_fingerprint:
        case AST::BuiltinKind::t_abi_slot_count:
            gen_fact(node);
            return;

        case AST::BuiltinKind::t_abi_symbol:
            gen_symbol(node);
            return;

        case AST::BuiltinKind::t_abi_slot_symbol:
            gen_slot_symbol(node);
            return;

        case AST::BuiltinKind::t_abi_bind_unchecked:
            gen_bind_unchecked(node);
            return;

        default:
            throw _ctx.error(fmt::format(
                "Builtin '{}' is not an ABI builtin {}",
                node.decl->builtin.value(), _ctx.function_context()));
    }
}

};
