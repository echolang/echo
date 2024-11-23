#include "Compiler/LLVM/Codegen/TypeDeclCodegen.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/TypeDeclNode.h"
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
void TypeDeclCodegen::gen_type_decl(AST::TypeDeclNode &node)
{
    // a generic struct template has type-parameter-typed properties and no concrete layout;
    // only its instantiations are lowered (lazily, in get_llvm_type)
    if (node.is_generic()) {
        return;
    }

    // one lowering of a TypeDeclNode, shared with build_struct_maps, which has normally already
    // reached this declaration - create_llvm_struct_decl answers from the structure table then
    // going through it rather than repeating it is what keeps the two paths from disagreeing about
    // what lowering a declaration means: a class also needs its heap block and its identity global,
    // and a second copy of the property loop had no idea about either
    _ctx.types->create_llvm_struct_decl(&node, *_ctx.current_cmp_unit);
}

void TypeDeclCodegen::gen_member_access(AST::MemberAccessNode &node)
{
    // the same lvalue path a member write uses, so a read and a write can never disagree
    // about which field they mean (todo/A3). a pointer-typed field carries its own explicit
    // deref node when it is read in value position. it also resolves the property, so the void
    // guard below reads the answer off the place rather than walking the base chain a second time
    auto place = _ctx.lvalues->gen_lvalue(node);

    if (place.storage_type.is_void()) {
        // the type-check pass should have reported an unknown/void member before codegen; if we
        // reach here it slipped through, so surface it with as much context as we have
        throw _ctx.error(fmt::format(
            "Cannot access member '{}' of void type {}",
            node.get_member_name().value(), _ctx.function_context()));
    }

    _ctx.value_stack.push(_ctx.lvalues->gen_load(place, node.get_member_name().value().c_str()));
}

void TypeDeclCodegen::gen_var(AST::VarNode &node)
{
    // get the LLVM value for this variable (should be an alloca instruction)
    auto it = _ctx.var_map.find(&node.decl());
    if (it == _ctx.var_map.end()) {
        throw _ctx.error(fmt::format(
            "Variable '{}' not found in variable map", node.decl().name()));
    }

    // push the alloca instruction (variable pointer) onto the stack
    _ctx.value_stack.push(it->second);
}

};
