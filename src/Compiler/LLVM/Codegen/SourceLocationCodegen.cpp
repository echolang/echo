#include "Compiler/LLVM/Codegen/SourceLocationCodegen.h"

#include "AST/ASTCoreTypes.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "Compiler/Fnv.h"
#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/Codegen/CodegenValue.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <vector>

namespace Compiler::LLVM
{

llvm::Value *SourceLocationCodegen::intern(const AST::SourceSite &site, const std::string &function)
{
    const AST::ValueType loc_type = _ctx.core_types().type(AST::CoreTypeKind::t_source_location);
    if (loc_type.is_unknown()) {
        throw _ctx.error(fmt::format(
            "{} is not bound {}",
            _ctx.core_types().spelling(AST::CoreTypeKind::t_source_location),
            _ctx.function_context()));
    }

    const AST::CoreSourceLocationLayout &layout = _ctx.core_source_location_layout();
    llvm::Type *agg = _ctx.types->get_llvm_type(loc_type, *_ctx.current_cmp_unit);
    auto *struct_ty = llvm::cast<llvm::StructType>(agg);
    const AST::ValueType view_type = _ctx.core_types().string_view_type();
    llvm::Type *view_ty = _ctx.types->get_llvm_type(view_type, *_ctx.current_cmp_unit);
    auto *view_struct = llvm::cast<llvm::StructType>(view_ty);

    auto view_of = [&](const std::string &text) -> llvm::Constant * {
        llvm::Constant *bytes = _ctx.builder->CreateGlobalStringPtr(text, "");
        llvm::Constant *size = llvm::ConstantInt::get(
            view_struct->getElementType(static_cast<unsigned>(layout.view_size_index)),
            text.size());

        std::vector<llvm::Constant *> fields(view_struct->getNumElements());
        for (unsigned i = 0; i < view_struct->getNumElements(); i++) {
            fields[i] = llvm::Constant::getNullValue(view_struct->getElementType(i));
        }
        fields[static_cast<unsigned>(layout.view_bytes_index)] = bytes;
        fields[static_cast<unsigned>(layout.view_size_index)] = size;
        return llvm::ConstantStruct::get(view_struct, fields);
    };

    std::vector<llvm::Constant *> fields(struct_ty->getNumElements());
    for (unsigned i = 0; i < struct_ty->getNumElements(); i++) {
        fields[i] = llvm::Constant::getNullValue(struct_ty->getElementType(i));
    }

    llvm::Type *line_ty = struct_ty->getElementType(static_cast<unsigned>(layout.line_index));
    fields[static_cast<unsigned>(layout.module_index)] = view_of(site.module);
    fields[static_cast<unsigned>(layout.file_index)] = view_of(site.file);
    fields[static_cast<unsigned>(layout.line_index)] = llvm::ConstantInt::get(line_ty, site.line);
    fields[static_cast<unsigned>(layout.column_index)] =
        llvm::ConstantInt::get(
            struct_ty->getElementType(static_cast<unsigned>(layout.column_index)), site.column);
    fields[static_cast<unsigned>(layout.function_index)] = view_of(function);

    llvm::Constant *init = llvm::ConstantStruct::get(struct_ty, fields);
    const uint64_t digest = Compiler::fnv1a64(
        site.file + "\0" + site.module + "\0" + function, Compiler::k_fnv_offset_basis);
    const std::string symbol = fmt::format(
        "__eco_src_{}_{}_{}", site.line, site.column, Compiler::to_hex(digest));

    if (llvm::GlobalVariable *existing = _ctx.current_module()->getGlobalVariable(symbol, true)) {
        return existing;
    }

    auto *global = new llvm::GlobalVariable(
        *_ctx.current_module(),
        struct_ty,
        true,
        llvm::GlobalValue::LinkOnceODRLinkage,
        init,
        symbol);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return global;
}

llvm::Value *SourceLocationCodegen::caller_address()
{
    if (_ctx.current_function == nullptr || !_ctx.current_function->tracks_caller) {
        return nullptr;
    }

    if (_ctx.current_function->caller_arg_index() >= _ctx.current_function->args.size()) {
        return nullptr;
    }

    AST::VarDeclNode *param =
        _ctx.current_function->args[_ctx.current_function->caller_arg_index()];
    auto found = _ctx.var_map.find(param);
    if (found == _ctx.var_map.end()) {
        return nullptr;
    }

    return _ctx.builder->CreateLoad(
        _ctx.opaque_ptr_type(), found->second, "source.caller.ptr");
}

void SourceLocationCodegen::gen_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind)
{
    const AST::ValueType loc_type = _ctx.core_types().type(AST::CoreTypeKind::t_source_location);
    llvm::Type *agg = _ctx.types->get_llvm_type(loc_type, *_ctx.current_cmp_unit);

    if (kind == AST::BuiltinKind::t_source_caller) {
        if (_ctx.current_function == nullptr || !_ctx.current_function->tracks_caller) {
            throw _ctx.error(fmt::format(
                "'{}' is only available in a function marked '#[caller]' {}",
                node.decl != nullptr ? node.decl->namespaced_func_name() : "caller",
                _ctx.function_context()));
        }

        llvm::Value *ptr = caller_address();
        if (ptr == nullptr) {
            throw _ctx.error(fmt::format(
                "'$__caller' has no allocation in scope {}", _ctx.function_context()));
        }

        _ctx.push(CodegenValue::aggregate(ptr, agg, Provenance::t_typed));
        return;
    }

    const AST::SourceSite site = AST::source_site_of(node);
    const std::string function = _ctx.current_function != nullptr
        ? _ctx.current_function->func_name()
        : std::string();
    _ctx.push(CodegenValue::aggregate(intern(site, function), agg, Provenance::t_typed));
}

void SourceLocationCodegen::abort_from_call(
    AST::FunctionCallExprNode &node,
    const std::string &headline,
    llvm::Value *detail_ptr,
    llvm::Value *detail_len
)
{
    const bool is_assert = node.decl != nullptr && node.decl->is_builtin()
        && AST::builtin_kind_for(node.decl->builtin.value()) == AST::BuiltinKind::t_assert;
    const size_t at_index = is_assert ? 2 : 1;

    llvm::Value *loc_addr = nullptr;

    const bool has_at = node.arguments.size() > at_index && node.arguments[at_index] != nullptr;
    const bool explicit_at = has_at && !node.arguments[at_index]->from_parameter_default;

    if (explicit_at) {
        node.arguments[at_index]->accept(*_ctx.visitor);
        loc_addr = _ctx.pop_value().value;
    }
    else if (llvm::Value *fwd = caller_address()) {
        loc_addr = fwd;
    }
    else if (_ctx.core_types().has(AST::CoreTypeKind::t_source_location)
        && _ctx.source_location_layout.has_value()) {
        // a default `$at` is interned at this `die` / `assert` token
        const AST::SourceSite site = AST::source_site_of(node);
        const std::string function = _ctx.current_function != nullptr
            ? _ctx.current_function->func_name()
            : std::string();
        loc_addr = intern(site, function);
    }

    if (loc_addr != nullptr && _ctx.source_location_layout.has_value()) {
        const AST::CoreSourceLocationLayout &layout = _ctx.core_source_location_layout();
        const AST::ValueType loc_type = _ctx.core_types().type(AST::CoreTypeKind::t_source_location);
        llvm::Type *agg = _ctx.types->get_llvm_type(loc_type, *_ctx.current_cmp_unit);
        const AST::ValueType view_type = _ctx.core_types().string_view_type();
        llvm::Type *view_ty = _ctx.types->get_llvm_type(view_type, *_ctx.current_cmp_unit);

        llvm::Value *file_view = _ctx.builder->CreateLoad(
            view_ty,
            _ctx.builder->CreateStructGEP(
                agg, loc_addr, static_cast<unsigned>(layout.file_index), "source.abort.file.slot"),
            "source.abort.file");
        llvm::Value *file_ptr = _ctx.builder->CreateExtractValue(
            file_view, { static_cast<unsigned>(layout.view_bytes_index) }, "source.abort.file.bytes");
        llvm::Value *file_len = _ctx.builder->CreateExtractValue(
            file_view, { static_cast<unsigned>(layout.view_size_index) }, "source.abort.file.len");
        llvm::Value *line = _ctx.builder->CreateLoad(
            llvm::Type::getInt32Ty(*_ctx.llvm_context),
            _ctx.builder->CreateStructGEP(
                agg, loc_addr, static_cast<unsigned>(layout.line_index), "source.abort.line.slot"),
            "source.abort.line");

        _ctx.abort->gen_abort_at(headline, detail_ptr, detail_len, file_ptr, file_len, line);
        return;
    }

    if (detail_ptr == nullptr) {
        _ctx.abort->gen_abort(headline, "", node.token_function_name);
        return;
    }

    _ctx.abort->gen_abort_dynamic(headline, detail_ptr, detail_len, node.token_function_name);
}

};
