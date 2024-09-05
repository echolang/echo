#include "Compiler/LLVM/Codegen/StructCodegen.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/StructNode.h"
#include "AST/AssignNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ReturnNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ASTPlaceExpr.h"

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

    // the same lvalue path a member write uses, so a read and a write can never disagree
    // about which field they mean (todo/A3). a pointer-typed field carries its own explicit
    // deref node when it is read in value position
    auto place = _ctx.lvalues->gen_lvalue(node);

    _ctx.value_stack.push(_ctx.builder->CreateLoad(
        _ctx.types->get_llvm_type(place.storage_type, *_ctx.current_cmp_unit),
        place.address,
        node.get_member_name().value()));
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

}
