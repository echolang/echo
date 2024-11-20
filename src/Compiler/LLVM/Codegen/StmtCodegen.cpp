#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ScopeNode.h"
#include "AST/ASTMangler.h"
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
void StmtCodegen::gen_scope(AST::ScopeNode &node)
{
    for (auto &child : node.children) {

        // skip function declarations
        if (child.has_type<AST::FunctionDeclNode>()) {
            continue;
        }

        // a statement must leave the value stack exactly as it found it. every value pushed by
        // a subexpression belongs to the parent that asked for it, so anything still on the
        // stack here is a leak - and a leak silently feeds the wrong value to a later pop
        const size_t depth_before = _ctx.value_stack.size();

        child.node()->accept(*_ctx.visitor);

        // an expression statement discards its value: `mem::free($p);` is a whole statement even
        // though the call has a result, and libc's memcpy/memset return a pointer nobody wants.
        // this is the one place a pushed value legitimately has no parent to claim it, so drop
        // it here rather than making callees lie about their return type
        if (AST::make_ref(child.node()).is_expression_node()) {
            while (_ctx.value_stack.size() > depth_before) {
                _ctx.value_stack.pop();
            }
        }

        // anything else leaving a value behind is still a genuine codegen bug
        assert(_ctx.value_stack.size() == depth_before && "statement leaked a value onto the stack");

        // after any return statement we need to terminate the block
        if (child.has_type<AST::ReturnNode>()) {
            break;
        }
    }
}

void StmtCodegen::gen_var_decl(AST::VarDeclNode &node)
{
    auto varname = node.name();
    llvm::Type* type = _ctx.types->get_llvm_type(node.type_node()->type, *_ctx.current_cmp_unit);

    // alloc the variable on the stack
    llvm::AllocaInst* alloca = _ctx.builder->CreateAlloca(type, nullptr, varname);

    // store the variable in the map
    _ctx.var_map[&node] = alloca;

    if (node.init_expr) {
        node.init_expr->accept(*_ctx.visitor);

        // check that the visited node pushed a value on the stack
        assert(_ctx.value_stack.size() > 0 && "No value on the stack");

        llvm::Value* init_value = _ctx.value_stack.top();
        _ctx.value_stack.pop();

        // the same conversion every assignment and member write uses. this path used to handle
        // only float/double, so an initializer that widened an integer stored the narrow value
        // straight into the wide slot and read back whatever else was in those bytes
        _ctx.builder->CreateStore(
            _ctx.types->coerce_value(init_value, node.init_expr->result_type(), node.type_node()->type, *_ctx.current_cmp_unit),
            alloca);
    }
}

void StmtCodegen::gen_function_decl(AST::FunctionDeclNode &node)
{
    // Skip compilation of generic function templates
    if (node.is_generic()) {
        return;
    }

    // sanity checks

    // 1. must have a body
    if (!node.body) {
        // if its an intrinsic function we can skip this
        if (node.intrinsic) {
            return;
        }

        // skip instantiated generic functions that don't have bodies yet.
        // this is a temporary measure while we implement proper body cloning
        // (is_generic() is exactly !type_parameters.empty(), so one check covers it)
        if (!node.is_generic()) {
            return;
        }

        assert(false);
        throw _ctx.error(fmt::format(
            "Function '{}' has no body associated with it.",
            node.func_name()
        ));
    }

    // track the enclosing function so codegen errors can name it. restored before returning.
    AST::FunctionDeclNode *prev_function = _ctx.current_function;
    _ctx.current_function = &node;

    // dump all function names in map
    auto funcid = _ctx.current_cmp_unit->function_table.get_function_id_by_name(AST::mangle_function_name(&node));
    auto func = _ctx.current_cmp_unit->function_table.get_llvm_function(funcid);

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", func);
    _ctx.builder->SetInsertPoint(entry);

    // create the arguments
    for (auto &arg : func->args()) {
        arg.setName(node.args[arg.getArgNo()]->name());
        llvm::AllocaInst *alloca = _ctx.builder->CreateAlloca(arg.getType(), nullptr, arg.getName());
        _ctx.builder->CreateStore(&arg, alloca);
        _ctx.var_map[node.args[arg.getArgNo()]] = alloca;
    }

    // a synthesized constructor arrives here like any other function - the struct parser builds its
    // body out of the same nodes a user would write, which is what keeps one implementation of the
    // member write and of the pointer re-seat
    node.body->accept(*_ctx.visitor);

    // Add a terminator if the block doesn't already have one
    if (!_ctx.builder->GetInsertBlock()->getTerminator()) {
        // If the function returns void, add a void return
        if (func->getReturnType()->isVoidTy()) {
            _ctx.builder->CreateRetVoid();
        } else {
            // For non-void functions without explicit return, this is an error
            // but we'll add a dummy return to keep LLVM happy
            llvm::Value *dummy_ret = llvm::UndefValue::get(func->getReturnType());
            _ctx.builder->CreateRet(dummy_ret);
        }
    }

    _ctx.current_function = prev_function;
}

void StmtCodegen::gen_return(AST::ReturnNode &node)
{
    // handle returns without an actual extression
    if (node.expr == nullptr) {
        _ctx.builder->CreateRetVoid();
        return;
    }

    node.expr->accept(*_ctx.visitor);

    // check if we actually got a value on the stack
    if (_ctx.value_stack.empty()) {
        _ctx.builder->CreateRetVoid();
        return;
    }

    llvm::Value *ret = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // a return fits its value to the declared return type through the same conversion table a
    // declaration, an assignment and a member write use - signedness lives on the ValueType and
    // not on the lowered llvm type, so `i8 -> i32` is a sign extend for int8 and a zero extend
    // for uint8. without it `return 0` in a `: float64` function reached CreateRet as an i32,
    // because a literal is typed where it is written and nothing there knows the return type
    //
    // result_type() may answer void (a binary expression whose operands differ does), which
    // coerce_value takes as "read the signedness off the value itself". a file-scope return has
    // no signature to answer to, and a value handed back from a `void` function is a semantic
    // error for the checker rather than a conversion to invent here
    if (_ctx.current_function != nullptr && !_ctx.current_function->get_return_type().is_void()) {
        ret = _ctx.types->coerce_value(
            ret, node.expr->result_type(), _ctx.current_function->get_return_type(),
            *_ctx.current_cmp_unit);
    }

    _ctx.builder->CreateRet(ret);
}

void StmtCodegen::gen_if_statement(AST::IfStatementNode &node)
{
    llvm::BasicBlock *if_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "if", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = nullptr;

    // condition
    node.condition->accept(*_ctx.visitor);
    llvm::Value *condition = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // if there is no else block we directly jump to the merge block
    if (!node.else_scope) {
        merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
        _ctx.builder->CreateCondBr(condition, if_block, merge_block);

        // if block
        _ctx.builder->SetInsertPoint(if_block);
        node.if_scope->accept(*_ctx.visitor);

        // if last instruction is not a terminator we need to add a branch to the merge block
        if (!_ctx.builder->GetInsertBlock()->getTerminator()) {
            _ctx.builder->CreateBr(merge_block);
        }

        // _ctx.builder->CreateBr(merge_block);
    } else {
        llvm::BasicBlock *else_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "else", _ctx.builder->GetInsertBlock()->getParent());

        _ctx.builder->CreateCondBr(condition, if_block, else_block);

        // if block
        _ctx.builder->SetInsertPoint(if_block);
        node.if_scope->accept(*_ctx.visitor);
        // _ctx.builder->CreateBr(merge_block);

        if (!_ctx.builder->GetInsertBlock()->getTerminator()) {
            merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
            _ctx.builder->CreateBr(merge_block);
        }

        // else block
        _ctx.builder->SetInsertPoint(else_block);
        node.else_scope->accept(*_ctx.visitor);
        // _ctx.builder->CreateBr(merge_block);

        if (!_ctx.builder->GetInsertBlock()->getTerminator()) {
            if (!merge_block) {
                merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());
            }
            _ctx.builder->CreateBr(merge_block);
        }
    }

    if (merge_block) {
        _ctx.builder->SetInsertPoint(merge_block);
    }
}

void StmtCodegen::gen_while_statement(AST::WhileStatementNode &node)
{
    llvm::BasicBlock *loop_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *body_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "body", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "merge", _ctx.builder->GetInsertBlock()->getParent());

    _ctx.builder->CreateBr(loop_block);

    // loop block
    _ctx.builder->SetInsertPoint(loop_block);
    node.condition->accept(*_ctx.visitor);
    llvm::Value *condition = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    _ctx.builder->CreateCondBr(condition, body_block, merge_block);

    // body block
    _ctx.builder->SetInsertPoint(body_block);
    node.loop_scope->accept(*_ctx.visitor);
    _ctx.builder->CreateBr(loop_block);

    // merge block
    _ctx.builder->SetInsertPoint(merge_block);
}

void StmtCodegen::gen_assign(AST::AssignNode &node)
{
    node.value_expr->accept(*_ctx.visitor);

    llvm::Value *new_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // one path for every left hand side shape, addressing exactly what the target names.
    // write-through is not decided here: `$p = 20` arrives as a deref of $p and lands on the
    // pointee, `$p:$ = &$b` arrives as $p itself and lands on the slot, re-seating it
    auto place = _ctx.lvalues->gen_lvalue(*node.target);

    _ctx.builder->CreateStore(
        _ctx.types->coerce_value(new_value, node.value_expr->result_type(), place.storage_type, *_ctx.current_cmp_unit),
        place.address);
}
}
