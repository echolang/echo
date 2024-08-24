#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ScopeNode.h"
#include "AST/ASTMangler.h"
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
void StmtCodegen::gen_scope(AST::ScopeNode &node)
{
    for (auto &child : node.children) {

        // skip function declarations
        if (child.has_type<AST::FunctionDeclNode>()) {
            continue;
        }

        child.node()->accept(*_ctx.visitor);

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

        // if the type is a float but our init_value is a double we need to convert it
        if (type->isFloatTy() && init_value->getType()->isDoubleTy()) {
            init_value = _ctx.builder->CreateFPTrunc(init_value, type);
        }
        else if (type->isDoubleTy() && init_value->getType()->isFloatTy()) {
            init_value = _ctx.builder->CreateFPExt(init_value, type);
        }

        _ctx.builder->CreateStore(init_value, alloca);
        _ctx.value_stack.pop();
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

        // Skip instantiated generic functions that don't have bodies yet
        // This is a temporary measure while we implement proper body cloning
        if (!node.is_generic() && node.type_parameters.empty()) {
            // This is likely an instantiated generic function without a body
            // Skip compilation for now
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

    // Auto-synthesized struct constructor only when there is no user-provided body
    bool is_struct_constructor = false;
    llvm::StructType *struct_type = nullptr;

    if (node.return_type && node.return_type->type.is_struct()) {
        struct_type = llvm::dyn_cast<llvm::StructType>(func->getReturnType());
        is_struct_constructor = (struct_type != nullptr && node.args.size() > 0 && node.body == nullptr);
    }

    if (is_struct_constructor) {
        // Generate struct constructor body
        // Allocate the struct on the stack
        llvm::AllocaInst *struct_alloca = _ctx.builder->CreateAlloca(struct_type, nullptr, "result");

        // Initialize struct fields with the constructor arguments
        for (size_t i = 0; i < node.args.size(); ++i) {
            // Get the argument variable
            auto arg_var = _ctx.var_map[node.args[i]];
            llvm::Value *arg_value = _ctx.builder->CreateLoad(arg_var->getAllocatedType(), arg_var);

            // Get pointer to the struct field
            std::vector<llvm::Value*> indices = {
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), 0),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*_ctx.llvm_context), i)
            };
            llvm::Value *field_ptr = _ctx.builder->CreateGEP(struct_type, struct_alloca, indices);

            // Store the argument value in the field
            _ctx.builder->CreateStore(arg_value, field_ptr);
        }

        // Load the struct and return it
        llvm::Value *struct_value = _ctx.builder->CreateLoad(struct_type, struct_alloca);
        _ctx.builder->CreateRet(struct_value);
    } else {
        // visit the function body for normal functions (including custom constructors)
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

    // Always evaluate the return expression during compilation
    // The stored result_type() may be void for generic expressions,
    // but during LLVM compilation the expression will be properly typed
    node.expr->accept(*_ctx.visitor);

    // Check if we actually got a value on the stack
    if (_ctx.value_stack.empty()) {
        _ctx.builder->CreateRetVoid();
        return;
    }

    llvm::Value *ret = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    _ctx.builder->CreateRet(ret);
}

void StmtCodegen::gen_if_statement(AST::IfStatementNode &node)
{
    llvm::BasicBlock *if_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "if", _ctx.builder->GetInsertBlock()->getParent());
    llvm::BasicBlock *merge_block = nullptr;

    // condition
    node.condition->accept(*_ctx.visitor);
    llvm::Value *condition = _ctx.value_stack.top();

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

void StmtCodegen::gen_var_mut(AST::VarMutNode &node)
{
    // Visit the value expression to get its LLVM IR value
    node.value_expr->accept(*_ctx.visitor);

    // Get the value from the stack
    llvm::Value* new_value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // Find the variable declaration
    if (node.var_decl == nullptr) {
        throw _ctx.error(fmt::format(
            "No variable declaration resolved for mutation of '{}' {}",
            node.name_full(), _ctx.function_context()));
    }

    // Get the allocated variable from the map
    auto var_iter = _ctx.var_map.find(node.var_decl);
    if (var_iter == _ctx.var_map.end()) {
        throw _ctx.error(fmt::format(
            "Variable '{}' has no allocation in scope {}",
            node.var_decl->name(), _ctx.function_context()));
    }

    llvm::AllocaInst* var = var_iter->second;
    llvm::Value* target = var;

    // Check if it's a pointer using ValueType.is_pointer()
    if (node.var_decl->type_node()->type.is_pointer()) {
        // For pointer variables, load the pointer first, then store through it
        target = _ctx.builder->CreateLoad(var->getAllocatedType(), var);

        // Get the target type (what the pointer points to) for type casting
        AST::ValueType target_type = node.var_decl->type_node()->type;
        target_type.set_pointer(false); // Remove pointer flag to get target type
        llvm::Type *target_llvm_type = _ctx.types->get_llvm_type(target_type, *_ctx.current_cmp_unit);

        // Cast the new value to the target type if necessary
        if (target_llvm_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
            new_value = _ctx.builder->CreateFPTrunc(new_value, target_llvm_type);
        } else if (target_llvm_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
            new_value = _ctx.builder->CreateFPExt(new_value, target_llvm_type);
        } else if (target_llvm_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
            new_value = _ctx.builder->CreateFPToSI(new_value, target_llvm_type);
        } else if (target_llvm_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
            new_value = _ctx.builder->CreateSIToFP(new_value, target_llvm_type);
        } else if (target_llvm_type->isIntegerTy() && new_value->getType()->isIntegerTy() &&
                   target_llvm_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
            if (target_llvm_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
                new_value = _ctx.builder->CreateSExt(new_value, target_llvm_type);
            } else {
                new_value = _ctx.builder->CreateTrunc(new_value, target_llvm_type);
            }
        }
    } else {
        // For non-pointer variables, cast to the variable type
        llvm::Type* var_type = var->getAllocatedType();

        if (var_type->isFloatTy() && new_value->getType()->isDoubleTy()) {
            new_value = _ctx.builder->CreateFPTrunc(new_value, var_type);
        } else if (var_type->isDoubleTy() && new_value->getType()->isFloatTy()) {
            new_value = _ctx.builder->CreateFPExt(new_value, var_type);
        } else if (var_type->isIntegerTy() && new_value->getType()->isFloatingPointTy()) {
            new_value = _ctx.builder->CreateFPToSI(new_value, var_type);
        } else if (var_type->isFloatingPointTy() && new_value->getType()->isIntegerTy()) {
            new_value = _ctx.builder->CreateSIToFP(new_value, var_type);
        } else if (var_type->isIntegerTy() && new_value->getType()->isIntegerTy() &&
                   var_type->getIntegerBitWidth() != new_value->getType()->getIntegerBitWidth()) {
            if (var_type->getIntegerBitWidth() > new_value->getType()->getIntegerBitWidth()) {
                new_value = _ctx.builder->CreateSExt(new_value, var_type);
            } else {
                new_value = _ctx.builder->CreateTrunc(new_value, var_type);
            }
        }
    }

    // Store the new value in the target
    _ctx.builder->CreateStore(new_value, target);
}
}
