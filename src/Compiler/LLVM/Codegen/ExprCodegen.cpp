#include "Compiler/LLVM/Codegen/ExprCodegen.h"
#include "eco.h"
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
#include "AST/ASTBuiltin.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/NullNode.h"

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

// the printf conversion `echo` prints a value with, or null for a type it has no format for.
// a table rather than an if-cascade so that adding a primitive is one line and cannot silently
// reuse a neighbouring width - `usize`/`isize` print as their concrete 64 bit width, which is
// what ECO_TARGET_POINTER_SIZE says on every target wired up today
static const char *printf_format_for(const AST::ValueType &type)
{
    if (!type.is_primitive()) {
        return nullptr;
    }

    switch (type.get_primitive_type())
    {
        case AST::ValueTypePrimitive::t_int8:
        case AST::ValueTypePrimitive::t_int16:
        case AST::ValueTypePrimitive::t_int32:
        case AST::ValueTypePrimitive::t_bool:
            return "%d\n";

        case AST::ValueTypePrimitive::t_int64:
        case AST::ValueTypePrimitive::t_isize:
            return "%lld\n";

        case AST::ValueTypePrimitive::t_uint8:
        case AST::ValueTypePrimitive::t_uint16:
        case AST::ValueTypePrimitive::t_uint32:
            return "%u\n";

        case AST::ValueTypePrimitive::t_uint64:
        case AST::ValueTypePrimitive::t_usize:
            return "%llu\n";

        case AST::ValueTypePrimitive::t_float32:
        case AST::ValueTypePrimitive::t_float64:
            return "%f\n";

        default:
            return nullptr;
    }
}
void ExprCodegen::gen_type_cast(AST::TypeCastNode &node)
{
    // visit the expression
    node.expr->accept(*_ctx.visitor);

    auto value = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // narrowing a nullable pointer to a borrow asserts the thing a borrow promises. under
    // opaque pointers the reinterpretation itself is free, so the trap is all there is to emit
    if (node.result_type().is_pointer() && !node.result_type().is_nullable()
        && node.expr->result_type().is_pointer() && node.expr->result_type().is_nullable()) {
        gen_null_assert(value);
    }

    // the conversion table lives on TypeLowering, shared with every declaration, assignment
    // and member write, so all of them agree on signedness
    _ctx.value_stack.push(_ctx.types->coerce_value(
        value, node.expr->result_type(), node.result_type(), *_ctx.current_cmp_unit));
}

void ExprCodegen::gen_var_ref(AST::VarRefNode &node)
{
    // gen_lvalue, not gen_place: any auto-deref this read needs is already an explicit
    // DerefExprNode above it, put there by the pointer adjustment pass. so a bare pointer
    // variable here means the pointer itself was asked for - which is what `$p:$` compiles to
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, node.is_var() ? node.get_var().decl().name().c_str() : "load"));
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

    // raw, not collapsed to the pointee: after the adjustment pass a pointer here means the
    // address really was asked for, through `:$`, and the pointer arm below needs to see it
    auto lhsret = node.lhs->result_type();
    auto rhsret = node.rhs->result_type();

    auto right = _ctx.value_stack.top();
    _ctx.value_stack.pop();
    auto left = _ctx.value_stack.top();
    _ctx.value_stack.pop();

    // everything reached through `:$` operates on the address itself: comparisons ask about
    // identity, and arithmetic is scaled by the pointee's size, never by bytes
    if (lhsret.is_pointer() || rhsret.is_pointer())
    {
        const bool both_pointers = lhsret.is_pointer() && rhsret.is_pointer();

        switch (node.op_node->op->type) {
            case Token::Type::t_logical_eq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpEQ(left, right));
                return;
            case Token::Type::t_logical_neq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpNE(left, right));
                return;
            case Token::Type::t_open_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpULT(left, right));
                return;
            case Token::Type::t_close_angle:
                _ctx.value_stack.push(_ctx.builder->CreateICmpUGT(left, right));
                return;
            case Token::Type::t_logical_leq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpULE(left, right));
                return;
            case Token::Type::t_logical_geq:
                _ctx.value_stack.push(_ctx.builder->CreateICmpUGE(left, right));
                return;

            case Token::Type::t_op_add:
            case Token::Type::t_op_sub:
            {
                if (both_pointers) {
                    if (node.op_node->op->type != Token::Type::t_op_sub) {
                        throw _ctx.error(fmt::format("two addresses cannot be added {}",
                            _ctx.function_context()));
                    }

                    // the distance between two addresses, counted in elements
                    _ctx.value_stack.push(_ctx.builder->CreatePtrDiff(
                        _ctx.types->get_llvm_type(AST::value_type_of(lhsret), *_ctx.current_cmp_unit),
                        left, right));
                    return;
                }

                // GEP over the pointee type does the scaling, so the offset counts elements
                llvm::Value *offset = right;
                if (node.op_node->op->type == Token::Type::t_op_sub) {
                    offset = _ctx.builder->CreateNeg(offset);
                }

                _ctx.value_stack.push(_ctx.builder->CreateGEP(
                    _ctx.types->get_llvm_type(AST::value_type_of(lhsret), *_ctx.current_cmp_unit),
                    left, { offset }, "ptroff"));
                return;
            }

            default:
                throw _ctx.error(fmt::format("unsupported binary operator '{}' for operands '{}' and '{}' {}",
                    node.op_node->token_literal.value(), lhsret.get_type_desciption(),
                    rhsret.get_type_desciption(), _ctx.function_context()));
        }
    }

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
    // a compiler builtin is answered here rather than called: it has no llvm::Function, so this
    // has to come before the function-table lookup further down
    if (node.decl && node.decl->is_builtin()) {
        gen_builtin_call(node);
        return;
    }

    if (node.token_function_name.value() == "echo") {

        for (auto &arg : node.arguments) {
            arg->accept(*_ctx.visitor);

            auto arg_value = _ctx.value_stack.top();
            _ctx.value_stack.pop();

            // the argument's own type, with no peeling. the adjustment pass already inserted the
            // auto-deref for a pointer read, so a value-position read has the pointee's type by
            // the time it gets here - reaching for value_type_of() as well peeled a second time
            // and printed a genuine address (a call returning ptr<T>, say) as though it were the
            // int at that address. anything still a pointer here really is an address, and echo
            // has no format for one
            auto result_type = arg->result_type();

            const char *format = printf_format_for(result_type);
            if (format == nullptr) {
                throw _ctx.error(fmt::format(
                    "Unsupported argument type '{}' for 'echo' {}",
                    result_type.get_type_desciption(), _ctx.function_context()));
            }

            std::vector<llvm::Value *> ArgsV = {
                _ctx.builder->CreateGlobalStringPtr(format),
                arg_value
            };

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
        // already rewritten into AddrOfExprNode by the coercion pass (FuncCallParser /
        // Monomorphizer), so accepting them leaves the variable's address on the stack,
        // so no per-argument kind sniffing is needed here
        std::vector<llvm::Value *> args;
        for (auto &arg : node.arguments) {
            arg->accept(*_ctx.visitor);
            args.push_back(_ctx.value_stack.top());
            _ctx.value_stack.pop();
        }

        llvm::Value *ret = _ctx.builder->CreateCall(func, args);

        // a void call produces no value. pushing one anyway left a void-typed entry that no
        // parent ever pops, so a `foo();` statement quietly grew the stack
        if (!ret->getType()->isVoidTy()) {
            _ctx.value_stack.push(ret);
        }
    }
}

void ExprCodegen::gen_builtin_call(AST::FunctionCallExprNode &node)
{
    const AST::FunctionDeclNode *decl = node.decl;

    // the builtin asks about a type, and that type is the instance's single type argument. an
    // un-instantiated template reaching here means the monomorphizer never resolved the call,
    // which is a compiler bug rather than a source error
    if (decl->instantiation_args.size() != 1) {
        throw _ctx.error(fmt::format(
            "Builtin '{}' expects exactly one type argument, got {} {}",
            decl->builtin.value(), decl->instantiation_args.size(), _ctx.function_context()));
    }

    const AST::ValueType &subject = decl->instantiation_args[0];
    llvm::Type *llvm_subject = _ctx.types->get_llvm_type(subject, *_ctx.current_cmp_unit);

    // the result is whatever the declaration promised - usize in the stdlib - so the constant
    // lands with the type the caller's arithmetic already expects
    llvm::Type *result_type = _ctx.types->get_llvm_type(decl->get_return_type(), *_ctx.current_cmp_unit);

    uint64_t value = 0;
    switch (AST::builtin_kind_for(decl->builtin.value()))
    {
        case AST::BuiltinKind::t_size_of:
            // getTypeAllocSize, not getTypeStoreSize: it includes tail padding, so it is the
            // stride between array elements. that is exactly what `alloc<T>(count)` and `$p:$[n]`
            // mean, and using the store size would under-allocate for any padded struct
            value = _ctx.layout().getTypeAllocSize(llvm_subject);
            break;

        case AST::BuiltinKind::t_align_of:
            value = _ctx.layout().getABITypeAlign(llvm_subject).value();
            break;
    }

    _ctx.value_stack.push(llvm::ConstantInt::get(result_type, value));
}

void ExprCodegen::gen_addr_of(AST::AddrOfExprNode &node)
{
    // `&E` is the address of E's slot, with no transparency peeling - gen_lvalue, not
    // gen_place. so `&$buf` on a `ptr<uint8>` yields the address of $buf itself
    _ctx.value_stack.push(_ctx.lvalues->gen_lvalue(*node.operand).address);
}

void ExprCodegen::gen_null_assert(llvm::Value *address)
{
#if ECO_DONT_CATCH_EXCEPTIONS || !defined(NDEBUG)
    llvm::Function *fn = _ctx.builder->GetInsertBlock()->getParent();

    llvm::Value *is_null = _ctx.builder->CreateICmpEQ(
        address,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(*_ctx.llvm_context, 0)),
        "isnull");

    llvm::BasicBlock *trap_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "null_trap", fn);
    llvm::BasicBlock *ok_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "not_null", fn);

    _ctx.builder->CreateCondBr(is_null, trap_block, ok_block);

    _ctx.builder->SetInsertPoint(trap_block);
    _ctx.builder->CreateCall(llvm::Intrinsic::getDeclaration(_ctx.current_module(), llvm::Intrinsic::trap));
    _ctx.builder->CreateUnreachable();

    _ctx.builder->SetInsertPoint(ok_block);
#endif
}

void ExprCodegen::gen_index(AST::IndexExprNode &node)
{
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, "elem"));
}

void ExprCodegen::gen_deref(AST::DerefExprNode &node)
{
    // gen_lvalue on the deref node itself resolves to the pointee's storage; loading it is
    // the read. keeping the address computation in LValueCodegen is what lets a deref appear
    // on the left of an assignment as readily as on the right
    _ctx.value_stack.push(_ctx.lvalues->gen_load(node, "deref"));
}

void ExprCodegen::gen_null(AST::NullNode &node)
{
    // every pointer is the same opaque `ptr` under llvm, so one null constant serves them all
    _ctx.value_stack.push(llvm::ConstantPointerNull::get(
        llvm::PointerType::get(*_ctx.llvm_context, 0)));
}

void ExprCodegen::gen_operator(AST::OperatorNode &node)
{
}
}
