#include "Compiler/LLVM/Codegen/ErasureCodegen.h"

#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTCoreTypes.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{

unsigned ErasureCodegen::require_property(const char *name)
{
    const AST::ComplexType::Property *prop = _ctx.core_types().property(
        AST::CoreTypeKind::t_erased, name);

    if (prop == nullptr) {
        throw _ctx.error(fmt::format(
            "'{}' has no property '${}' {}",
            _ctx.core_types().spelling(AST::CoreTypeKind::t_erased),
            name,
            _ctx.function_context()));
    }

    return static_cast<unsigned>(prop->index);
}

unsigned ErasureCodegen::object_index()
{
    return require_property("object");
}

unsigned ErasureCodegen::release_index()
{
    return require_property("release");
}

llvm::Value *ErasureCodegen::load_erased(AST::FunctionCallExprNode &node)
{
    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *addr = _ctx.pop();
    const AST::ValueType argument_type = node.arguments[0]->result_type();
    llvm::Type *erased_type = _ctx.types->get_llvm_type(
        AST::value_type_of(argument_type), *_ctx.current_cmp_unit);

    if (argument_type.is_pointer()) {
        return _ctx.builder->CreateLoad(erased_type, addr, "erased");
    }

    return addr;
}

void ErasureCodegen::gen_from(AST::FunctionCallExprNode &node)
{
    const AST::FunctionDeclNode *decl = node.decl;

    if (decl->instantiation_args.size() != 1 || node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format(
            "'{}::from' expects a class handle {}",
            _ctx.core_types().spelling(AST::CoreTypeKind::t_erased),
            _ctx.function_context()));
    }

    const AST::ValueType class_type = AST::ValueType::make_mutable(decl->instantiation_args[0]);
    node.arguments[0]->accept(*_ctx.visitor);
    llvm::Value *handle = _ctx.pop();

    const AST::ValueType argument_type = node.arguments[0]->result_type();
    if (argument_type.is_pointer()) {
        handle = _ctx.builder->CreateLoad(_ctx.opaque_ptr_type(), handle, "handle");
    }

    // the argument is a borrow, so this retain is the erased value's own reference
    _ctx.classes->gen_erased_retain(handle);

    llvm::Function *thunk = _ctx.classes->get_or_create_release_thunk(class_type);
    llvm::Type *agg_type = _ctx.types->get_llvm_type(decl->get_return_type(), *_ctx.current_cmp_unit);

    llvm::Value *agg = llvm::UndefValue::get(agg_type);
    agg = _ctx.builder->CreateInsertValue(agg, handle, { object_index() }, "erased.obj");
    agg = _ctx.builder->CreateInsertValue(agg, thunk, { release_index() }, "erased.release");
    _ctx.push(agg);
}

void ErasureCodegen::gen_retain(AST::FunctionCallExprNode &node)
{
    if (node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format("'erased_retain' takes one argument {}", _ctx.function_context()));
    }

    llvm::Value *agg = load_erased(node);
    llvm::Value *object = _ctx.builder->CreateExtractValue(agg, { object_index() }, "erased.obj");
    _ctx.classes->gen_erased_retain(object);
}

void ErasureCodegen::gen_release(AST::FunctionCallExprNode &node)
{
    if (node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format("'erased_release' takes one argument {}", _ctx.function_context()));
    }

    llvm::Value *agg = load_erased(node);
    llvm::Value *object = _ctx.builder->CreateExtractValue(agg, { object_index() }, "erased.obj");
    llvm::Value *thunk = _ctx.builder->CreateExtractValue(agg, { release_index() }, "erased.release");
    _ctx.classes->gen_call_release_thunk(object, thunk);
}

void ErasureCodegen::gen_assume(AST::FunctionCallExprNode &node)
{
    if (node.arguments.size() != 1 || node.arguments[0] == nullptr) {
        throw _ctx.error(fmt::format("'assume' takes one argument {}", _ctx.function_context()));
    }

    llvm::Value *agg = load_erased(node);
    llvm::Value *object = _ctx.builder->CreateExtractValue(agg, { object_index() }, "erased.obj");
    _ctx.push(_ctx.classes->gen_erased_retain(object));
}

};
