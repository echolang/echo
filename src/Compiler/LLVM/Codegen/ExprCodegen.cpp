#include "Compiler/LLVM/Codegen/ExprCodegen.h"
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
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>
#include <stdexcept>
#include <vector>

namespace Compiler::LLVM
{
void ExprCodegen::gen_type_cast(AST::TypeCastNode &node)
{
    // visit the expression
    node.expr->accept(*_ctx.visitor);

    // create a new value with the new type
    auto new_type = node.result_type();
    auto old_type = node.expr->result_type();

    auto new_llvm_type = _ctx.types->get_llvm_type(new_type.get_primitive_type());

    auto value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // if the types are identical we don't need to do anything
    if (old_type == new_type) {
        _ctx.value_stack.push(value);
        return;
    }

    // convert the value to a floating point type
    if (new_type.is_floating_type()) {
        if (old_type.is_integer_type()) {
            if (old_type.is_signed_integer()) {
                value = _ctx.builder->CreateSIToFP(value, new_llvm_type);
            } else {
                value = _ctx.builder->CreateUIToFP(value, new_llvm_type);
            }
        }
        // cast to another floating point type simply requires an extension or truncation
        else if (old_type.is_floating_type()) {
            if (old_type.get_primitive_type() == AST::ValueTypePrimitive::t_float32) {
                value = _ctx.builder->CreateFPExt(value, new_llvm_type);
            } else {
                value = _ctx.builder->CreateFPTrunc(value, new_llvm_type);
            }
        }
        // cast to a boolean type
        else if (old_type.is_boolean_type()) {
            value = _ctx.builder->CreateUIToFP(value, new_llvm_type);
        }
        else {
            throw _ctx.error(fmt::format("unsupported type cast from '{}' to '{}' {}",
                old_type.get_type_desciption(), new_type.get_type_desciption(), _ctx.function_context()));
        }
    }

    else if (new_type.is_integer_type()) {
        if (old_type.is_floating_type()) {
            if (new_type.is_signed_integer()) {
                value = _ctx.builder->CreateFPToSI(value, new_llvm_type);
            } else {
                value = _ctx.builder->CreateFPToUI(value, new_llvm_type);
            }
        }
        // cast to another integer type
        else if (old_type.is_integer_type()) {
            // any int -> signed int
            if (new_type.is_signed_integer()) {
                // uint -> int
                if (old_type.is_same_size(new_type) && old_type.is_unsigned_integer()) {
                    value = _ctx.builder->CreateIntCast(value, new_llvm_type, true);
                }
                // int8 -> int32 (smaller -> larger)
                else if (old_type.will_fit_into(new_type)) {
                    value = _ctx.builder->CreateSExt(value, new_llvm_type);
                }
                // int32 -> int8 (larger -> smaller)
                else {
                    value = _ctx.builder->CreateTrunc(value, new_llvm_type);
                }
            }
            // any int -> unsigned int
            else {
                // int -> uint
                if (old_type.is_same_size(new_type) && old_type.is_signed_integer()) {
                    value = _ctx.builder->CreateIntCast(value, new_llvm_type, false);
                }
                // uint8 -> uint32 (smaller -> larger)
                else if (old_type.will_fit_into(new_type)) {
                    value = _ctx.builder->CreateZExt(value, new_llvm_type);
                }
                // uint32 -> uint8 (larger -> smaller)
                else {
                    value = _ctx.builder->CreateTrunc(value, new_llvm_type);
                }
            }
        }
        // cast to a boolean type
        else if (old_type.is_boolean_type()) {
            value = _ctx.builder->CreateZExt(value, new_llvm_type);
        }
        else {
            throw _ctx.error(fmt::format("unsupported type cast from '{}' to '{}' {}",
                old_type.get_type_desciption(), new_type.get_type_desciption(), _ctx.function_context()));
        }
    }

    else if (new_type.is_boolean_type()) {
        if (old_type.is_integer_type()) {
            value = _ctx.builder->CreateICmpNE(value, llvm::ConstantInt::get(*_ctx.llvm_context, llvm::APInt(1, 0, false)));
        }
        else if (old_type.is_floating_type()) {
            value = _ctx.builder->CreateFCmpONE(value, llvm::ConstantFP::get(*_ctx.llvm_context, llvm::APFloat(0.0)));
        }
        else {
            throw _ctx.error(fmt::format("unsupported type cast from '{}' to '{}' {}",
                old_type.get_type_desciption(), new_type.get_type_desciption(), _ctx.function_context()));
        }
    }

    else {
        throw std::runtime_error("Unsupported type cast");
    }

    // push the new value on the stack
    _ctx.value_stack.push(value);
}

void ExprCodegen::gen_var_ref(AST::VarRefNode &node)
{
    if (node.is_var()) {
        // Handle regular variable reference
        auto &var_node = node.get_var();

        // Get the LLVM value for this variable (should be an alloca instruction)
        auto it = _ctx.var_map.find(&var_node.decl());
        if (it == _ctx.var_map.end()) {
            throw _ctx.error(fmt::format(
                "Variable '{}' not found in variable map", var_node.decl().name()));
        }

        llvm::Value *var_ptr = it->second;

        // Check if this is a pointer variable using ValueType.is_pointer()
        if (var_node.decl().type_node()->type.is_pointer()) {
            // For pointer variables, load the pointer first, then load the value it points to
            llvm::Type *pointer_type = _ctx.types->get_llvm_type(var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
            llvm::Value *pointer_value = _ctx.builder->CreateLoad(pointer_type, var_ptr, var_node.decl().name() + "_ptr");

            // Get the target type (what the pointer points to)
            AST::ValueType target_type = var_node.decl().type_node()->type;
            target_type.set_pointer(false); // Remove pointer flag to get target type
            llvm::Type *target_llvm_type = _ctx.types->get_llvm_type(target_type, *_ctx.current_cmp_unit);

            // Dereference the pointer to get the actual value
            llvm::Value *dereferenced_value = _ctx.builder->CreateLoad(target_llvm_type, pointer_value, var_node.decl().name());
            _ctx.value_stack.push(dereferenced_value);
        } else {
            // For non-pointer variables, just load the value normally
            llvm::Type *var_type = _ctx.types->get_llvm_type(var_node.decl().type_node()->type, *_ctx.current_cmp_unit);
            llvm::Value *loaded_value = _ctx.builder->CreateLoad(var_type, var_ptr, var_node.decl().name());
            _ctx.value_stack.push(loaded_value);
        }
    }
    else {
        throw _ctx.error("Unknown VarRef target type");
    }
}

void ExprCodegen::gen_literal_float(AST::LiteralFloatExprNode &node)
{
    if (node.get_effective_primitive_type() == AST::ValueTypePrimitive::t_float64) {
        _ctx.value_stack.push(llvm::ConstantFP::get(*_ctx.llvm_context, llvm::APFloat(node.double_value())));
    } else {
        _ctx.value_stack.push(llvm::ConstantFP::get(*_ctx.llvm_context, llvm::APFloat(node.float_value())));
    }
}

void ExprCodegen::gen_literal_int(AST::LiteralIntExprNode &node)
{
    auto type = node.result_type().get_primitive_type();
    auto value = node.uint64_value();

    auto int_size = AST::get_integer_size(type);

    // push an integer constant on the stack
    _ctx.value_stack.push(llvm::ConstantInt::get(*_ctx.llvm_context, llvm::APInt(int_size.size * 8, value, int_size.is_signed)));
}

void ExprCodegen::gen_literal_bool(AST::LiteralBoolExprNode &node)
{
    if (node.get_bool_value()) {
        _ctx.value_stack.push(llvm::ConstantInt::getTrue(*_ctx.llvm_context));
    } else {
        _ctx.value_stack.push(llvm::ConstantInt::getFalse(*_ctx.llvm_context));
    }
}

void ExprCodegen::gen_literal_string(AST::LiteralStringExprNode &node)
{
}

void ExprCodegen::gen_binary_expr(AST::BinaryExprNode &node)
{
    node.lhs->accept(*_ctx.visitor);
    node.rhs->accept(*_ctx.visitor);

    auto lhsret = node.lhs->result_type();
    auto rhsret = node.rhs->result_type();

    auto right = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    auto left = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    if (lhsret.is_integer_type() && rhsret.is_integer_type())
    {
        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                _ctx.value_stack.push(_ctx.builder->CreateAdd(left, right));
                break;
            case Token::Type::t_op_sub:
                _ctx.value_stack.push(_ctx.builder->CreateSub(left, right));
                break;
            case Token::Type::t_op_mul:
                _ctx.value_stack.push(_ctx.builder->CreateMul(left, right));
                break;
            case Token::Type::t_op_div:
                _ctx.value_stack.push(_ctx.builder->CreateSDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                _ctx.value_stack.push(_ctx.builder->CreateSRem(left, right));
                break;
            case Token::Type::t_op_pow:
                {
                    // im kinda just copying the behavior of C with clang here
                    // cast all values to double and then call the pow intrinsic
                    // cast the result back to the original type
                    std::vector<llvm::Type *> arg_type;
                    arg_type.push_back(llvm::Type::getDoubleTy(*_ctx.llvm_context));
                    arg_type.push_back(llvm::Type::getDoubleTy(*_ctx.llvm_context));

                    llvm::Function *fun = llvm::Intrinsic::getDeclaration(_ctx.current_module(), llvm::Intrinsic::pow, arg_type);
                    std::vector<llvm::Value *> args;
                    args.push_back(_ctx.builder->CreateSIToFP(left, llvm::Type::getDoubleTy(*_ctx.llvm_context)));
                    args.push_back(_ctx.builder->CreateSIToFP(right, llvm::Type::getDoubleTy(*_ctx.llvm_context)));

                    llvm::Value *result = _ctx.builder->CreateCall(fun, args);
                    _ctx.value_stack.push(_ctx.builder->CreateFPToSI(result, llvm::Type::getInt32Ty(*_ctx.llvm_context)));
                }
                break;
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpNE(left, right));
                break;
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSGT(left, right));
                break;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpSLE(left, right));
                break;
            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }
    else if (lhsret.is_boolean_type() && rhsret.is_boolean_type())
    {
        switch (node.op_node->op->type) {
            case Token::Type::t_logical_and:
                _ctx.value_stack.push(_ctx.builder->CreateAnd(left, right));
                break;
            case Token::Type::t_logical_or:
                _ctx.value_stack.push(_ctx.builder->CreateOr(left, right));
                break;
            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }
    else if (lhsret.is_floating_type() || rhsret.is_floating_type())
    {
        // Promote both sides to a common floating type (double if any operand is double)
        bool use_double = lhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64) ||
                          rhsret.is_primitive_of_type(AST::ValueTypePrimitive::t_float64);

        auto promote_to_fp = [&](llvm::Value *value, const AST::ValueType &vt) {
            if (vt.is_floating_type()) {
                if (use_double && vt.is_primitive_of_type(AST::ValueTypePrimitive::t_float32)) {
                    return _ctx.builder->CreateFPExt(value, llvm::Type::getDoubleTy(*_ctx.llvm_context));
                }
                return value;
            }

            // integer/boolean -> float/double
            if (use_double) {
                return _ctx.builder->CreateSIToFP(value, llvm::Type::getDoubleTy(*_ctx.llvm_context));
            }
            return _ctx.builder->CreateSIToFP(value, llvm::Type::getFloatTy(*_ctx.llvm_context));
        };

        left = promote_to_fp(left, lhsret);
        right = promote_to_fp(right, rhsret);

        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                _ctx.value_stack.push(_ctx.builder->CreateFAdd(left, right));
                break;
            case Token::Type::t_op_sub:
                _ctx.value_stack.push(_ctx.builder->CreateFSub(left, right));
                break;
            case Token::Type::t_op_mul:
                _ctx.value_stack.push(_ctx.builder->CreateFMul(left, right));
                break;
            case Token::Type::t_op_div:
                _ctx.value_stack.push(_ctx.builder->CreateFDiv(left, right));
                break;
            case Token::Type::t_op_mod:
                _ctx.value_stack.push(_ctx.builder->CreateFRem(left, right));
                break;
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOEQ(left, right));
                break;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpONE(left, right));
                break;
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOGT(left, right));
                break;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOLT(left, right));
                break;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOGE(left, right));
                break;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(_ctx.builder->CreateFCmpOLE(left, right));
                break;


            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }
    else {
        throw std::runtime_error("Unsupported binary operator");
    }
}

void ExprCodegen::gen_unary_expr(AST::UnaryExprNode &node)
{
    node.expr->accept(*_ctx.visitor);

    auto value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    auto type = node.expr->result_type();

    switch (node.token_operator.type()) {
        case Token::Type::t_op_sub:
            if (type.is_floating_type()) {
                _ctx.value_stack.push(_ctx.builder->CreateFNeg(value));
            }
            else if (type.is_integer_type()) {
                _ctx.value_stack.push(_ctx.builder->CreateNeg(value));
            }
            else {
                throw _ctx.error(fmt::format("unary '-' is not supported for operand type '{}' {}",
                    type.get_type_desciption(), _ctx.function_context()));
            }
            break;
        default:
            throw _ctx.error(fmt::format("unsupported unary operator '{}' {}",
                node.token_operator.value(), _ctx.function_context()));
    }
}

void ExprCodegen::gen_function_call(AST::FunctionCallExprNode &node)
{
    if (node.token_function_name.value() == "echo") {

        for (auto &arg : node.arguments) {
            arg->accept(*_ctx.visitor);

            auto arg_value = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            auto result_type = arg->result_type();

            // printf each argument value
            std::vector<llvm::Value *> ArgsV;


            if (
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int8) ||
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int16) ||
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int32)
            ) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%d\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int64)) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%lld\n"));
                ArgsV.push_back(arg_value);
            }
            else if (
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint8) ||
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint16) ||
                result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint32)
            ) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%u\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_uint64)) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%llu\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_float32)) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%f\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_float64)) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%f\n"));
                ArgsV.push_back(arg_value);
            }
            else if (result_type.is_primitive_of_type(AST::ValueTypePrimitive::t_bool)) {
                ArgsV.push_back(_ctx.builder->CreateGlobalStringPtr("%d\n"));
                ArgsV.push_back(arg_value);
            }
            else {
                throw _ctx.error(fmt::format(
                    "Unsupported argument type '{}' for 'echo' {}",
                    result_type.get_type_desciption(), _ctx.function_context()));
            }

            _ctx.builder->CreateCall(_ctx.current_module()->getFunction("printf"), ArgsV);
        }
    }

    else
    {
        // locate the function
        auto funcid = _ctx.current_cmp_unit->function_table.get_function_id(node.decl);
        llvm::Function *func = _ctx.current_cmp_unit->function_table.get_llvm_function(funcid);

        // look for the function in the other modules
        if (!func) {
            for (auto &cmp_unit : _ctx.cmp_units) {
                if (cmp_unit.get() == _ctx.current_cmp_unit) {
                    continue;
                }

                auto funcid = cmp_unit->function_table.get_function_id(node.decl);
                func = cmp_unit->function_table.get_llvm_function(funcid);

                if (func) {
                    break;
                }
            }
        }

        if (!func) {
            throw _ctx.error(fmt::format(
                "No generated function found for call to '{}' {}",
                node.decl ? node.decl->func_name() : node.token_function_name.value(),
                _ctx.function_context()));
        }

        // evaluate every argument uniformly. arguments bound to pointer parameters were
        // already rewritten into VarPtrExprNode by the coercion pass (FuncCallParser /
        // Monomorphizer), so accepting them leaves the variable's address on the stack,
        // so no per-argument kind sniffing is needed here
        std::vector<llvm::Value *> args;
        for (auto &arg : node.arguments) {
            arg->accept(*_ctx.visitor);
            args.push_back(_ctx.value_stack.top());
            _ctx.value_stack.pop();
        }

        llvm::Value *ret = _ctx.builder->CreateCall(func, args);
        _ctx.value_stack.push(ret);

    }
}

void ExprCodegen::gen_var_ptr(AST::VarPtrExprNode &node)
{
    // Get a pointer to the variable referenced by var_ref
    // We need to handle different types of variable references

    if (node.var_ref->is_var()) {
        // Handle regular variable reference - get the pointer (alloca)
        auto &var_node = node.var_ref->get_var();

        // Get the LLVM alloca instruction for this variable
        auto it = _ctx.var_map.find(&var_node.decl());
        if (it == _ctx.var_map.end()) {
            throw _ctx.error(fmt::format(
                "Variable '{}' not found in variable map", var_node.decl().name()));
        }

        // Push the alloca instruction (pointer to the variable) onto the stack
        _ctx.value_stack.push(it->second);
    }
    else {
        throw _ctx.error("Unknown VarRef target type in VarPtrExpr");
    }
}

void ExprCodegen::gen_null(AST::NullNode &node)
{
}

void ExprCodegen::gen_operator(AST::OperatorNode &node)
{
}
}
