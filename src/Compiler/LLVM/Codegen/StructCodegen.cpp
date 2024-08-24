#include "Compiler/LLVM/Codegen/StructCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/StructNode.h"
#include "AST/MemberMutNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ReturnNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/VarMutNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Compiler::LLVM
{
void StructCodegen::gen_struct_decl(AST::StructDeclNode &node)
{
    // a generic struct template has type-parameter-typed properties and no concrete layout;
    // only its instantiations are lowered (lazily, in get_llvm_type).
    if (node.is_generic()) {
        return;
    }

    if (!node.name_token.has_value()) {
        assert(false);
        throw _ctx.error("Anonymous struct declarations are not yet supported.");
    }

    auto struct_name = node.struct_name();

    // Check if this struct is already defined in the structure table
    if (_ctx.current_cmp_unit->structure_table->get_structure_id(&node) != 0) {
        // Already defined, skip
        return;
    }

    // Create an opaque struct type first and register it immediately
    llvm::StructType *llvm_struct_type = llvm::StructType::create(*_ctx.llvm_context, struct_name);
    _ctx.current_cmp_unit->structure_table->push_structure(&node, llvm_struct_type);

    // Now collect member types for LLVM struct (other structs should be resolvable now)
    std::vector<llvm::Type *> member_types;
    for (const auto &prop : node.properties()) {
        llvm::Type *llvm_type = _ctx.types->get_llvm_type(prop->type_node()->type, *_ctx.current_cmp_unit);
        if (!llvm_type) {
            assert(false);
            throw _ctx.error(fmt::format(
                "Unknown type for field '{}' in struct '{}'.",
                prop->name(), struct_name
            ));
        }
        member_types.push_back(llvm_type);
    }

    // Set the body of the struct type
    llvm_struct_type->setBody(member_types);
}

llvm::Value *StructCodegen::gen_struct_ptr(const AST::NodeReference &base)
{
    // a variable reference is the root of a chain: its storage holds the struct, unless the
    // variable is itself a pointer, in which case the storage holds the struct's address and
    // we load once to get to the struct
    if (base.has_type<AST::VarRefNode>()) {
        auto &var_ref = base.get<AST::VarRefNode>();
        if (var_ref.is_var()) {
            auto &var_node = var_ref.get_var();
            var_node.accept(*_ctx.visitor);
            llvm::Value *ptr = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            if (var_node.decl().type_node()->type.is_pointer()) {
                llvm::Type *pointer_type = _ctx.types->get_llvm_type(var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
                ptr = _ctx.builder->CreateLoad(pointer_type, ptr, var_node.decl().name() + "_deref");
            }
            return ptr;
        }

        base.node()->accept(*_ctx.visitor);
        llvm::Value *ptr = _ctx.value_stack.top();
        _ctx.value_stack.pop();
        return ptr;
    }

    // a nested member access: recurse to get a pointer to that member, which is itself a struct
    if (base.has_type<AST::MemberAccessNode>()) {
        return gen_member_ptr(base.get<AST::MemberAccessNode>());
    }

    // any other expression is expected to leave a pointer on the stack
    base.node()->accept(*_ctx.visitor);
    llvm::Value *ptr = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    return ptr;
}

llvm::Value *StructCodegen::gen_member_ptr(AST::MemberAccessNode &node)
{
    llvm::Value *struct_ptr = gen_struct_ptr(node.get_base_node());
    if (!struct_ptr) {
        throw _ctx.error("No base value for member access");
    }

    // the base type tells us which struct we are indexing into
    AST::ValueType base_type;
    if (node.get_base_node().has_type<AST::VarRefNode>()) {
        base_type = node.get_base_node().get<AST::VarRefNode>().result_type();
    } else if (node.get_base_node().has_type<AST::MemberAccessNode>()) {
        base_type = node.get_base_node().get<AST::MemberAccessNode>().result_type();
    } else {
        throw _ctx.error("Unsupported base type for member access");
    }

    if (!base_type.is_struct() || !base_type.get_complex_type()) {
        throw _ctx.error("Invalid base type for member access");
    }

    auto complex = base_type.get_complex_type();
    auto member_name = node.get_member_name().value();

    // find the member index
    size_t member_index = 0;
    bool found = false;
    for (size_t i = 0; i < complex->property_count(); ++i) {
        if (complex->get_property(i).name == member_name) {
            member_index = i;
            found = true;
            break;
        }
    }

    if (!found) {
        throw _ctx.error(fmt::format(
            "Member '{}' not found in struct '{}' {}",
            member_name, complex->name.value_or("<anonymous>"), _ctx.function_context()));
    }

    // get the struct type from the structure table
    auto struct_id = _ctx.current_cmp_unit->structure_table->get_structure_id(complex);
    if (struct_id == 0) {
        throw _ctx.error("Struct not found in structure table");
    }

    auto &structure = _ctx.current_cmp_unit->structure_table->get_structure(struct_id);

    std::vector<llvm::Value *> indices = {
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), member_index)
    };

    return _ctx.builder->CreateGEP(structure.llvm_struct, struct_ptr, indices, member_name + "_ptr");
}

void StructCodegen::gen_member_access(AST::MemberAccessNode &node)
{
    auto result_type = node.result_type();
    if (result_type.is_void()) {
        // the type-check pass should have reported an unknown/void member before codegen; if we
        // reach here it slipped through, so surface it with as much context as we have.
        throw _ctx.error(fmt::format(
            "Cannot access member '{}' of void type {}",
            node.get_member_name().value(), _ctx.function_context()));
    }

    llvm::Value *member_ptr = gen_member_ptr(node);

    // load the value from the member
    llvm::Value *member_value = _ctx.builder->CreateLoad(
        _ctx.types->get_llvm_type(result_type, *_ctx.current_cmp_unit), member_ptr, node.get_member_name().value());

    _ctx.value_stack.push(member_value);
}

void StructCodegen::gen_var(AST::VarNode &node)
{
    // Get the LLVM value for this variable (should be an alloca instruction)
    auto it = _ctx.var_map.find(&node.decl());
    if (it == _ctx.var_map.end()) {
        throw _ctx.error(fmt::format(
            "Variable '{}' not found in variable map", node.decl().name()));
    }

    // Push the alloca instruction (variable pointer) onto the stack
    _ctx.value_stack.push(it->second);
}

void StructCodegen::gen_member_mut(AST::MemberMutNode &node)
{
    // Visit the value expression first to get the new value
    node.value_expr->accept(*_ctx.visitor);

    if (_ctx.value_stack.empty()) {
        throw _ctx.error("No value on stack for member mutation");
    }

    llvm::Value *new_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // Get the member access node and generate the pointer to the member
    auto &member_access = *node.member_access;

    // resolve a pointer to the addressed field, walking the chain to any depth
    llvm::Value *member_ptr = gen_member_ptr(member_access);

    // get the member type for potential type conversion
    auto result_type = member_access.result_type();
    llvm::Type *member_llvm_type = _ctx.types->get_llvm_type(result_type, *_ctx.current_cmp_unit);

    // cast the new value to the member's type if necessary
    if (member_llvm_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
        new_value = _ctx.builder->CreateFPTrunc(new_value, member_llvm_type);
    } else if (member_llvm_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
        new_value = _ctx.builder->CreateFPExt(new_value, member_llvm_type);
    } else if (member_llvm_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
        new_value = _ctx.builder->CreateFPToSI(new_value, member_llvm_type);
    } else if (member_llvm_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
        new_value = _ctx.builder->CreateSIToFP(new_value, member_llvm_type);
    } else if (member_llvm_type->isIntegerTy() && new_value->getType()->isIntegerTy() &&
               member_llvm_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
        if (member_llvm_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
            new_value = _ctx.builder->CreateSExt(new_value, member_llvm_type);
        } else {
            new_value = _ctx.builder->CreateTrunc(new_value, member_llvm_type);
        }
    }

    // store the new value in the member
    _ctx.builder->CreateStore(new_value, member_ptr);
}
}
