#include "Compiler/LLVM/Codegen/SimdCodegen.h"

#include "Compiler/LLVM/Codegen/IntrinsicResolution.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTSimd.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "Token.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{
    llvm::Value *SimdCodegen::splat(llvm::Value *scalar, llvm::FixedVectorType *vec_ty)
    {
        llvm::Value *poison = llvm::PoisonValue::get(vec_ty);
        llvm::Value *insert = _ctx.builder->CreateInsertElement(
            poison, scalar, _ctx.builder->getInt64(0), "simd.splat.lane");

        const unsigned n = vec_ty->getNumElements();
        llvm::SmallVector<int, 16> zeroes(n, 0);

        return _ctx.builder->CreateShuffleVector(insert, poison, zeroes, "simd.splat");
    }

    llvm::Align SimdCodegen::packed_alignment(const AST::ValueType &vec)
    {
        llvm::Type *element = _ctx.types->get_llvm_type(
            vec.simd_element(), *_ctx.current_cmp_unit);

        return _ctx.layout().getABITypeAlign(element);
    }

    void SimdCodegen::gen_binary(AST::BinaryExprNode &node, const AST::ValueType &lhs, const AST::ValueType &rhs, llvm::Value *left, llvm::Value *right)
    {
        const auto unlowered = [&]() {
            return _ctx.error(fmt::format(
                "compiler bug: operator '{}' has no lowering for operands '{}' and '{}', but "
                "AST::binary_has_builtin_meaning accepted them {}",
                node.op_node->token_literal.value(), lhs.get_type_desciption(),
                rhs.get_type_desciption(), _ctx.function_context()));
        };

        // the pair was accepted by AST::binary_operand_refusal, which asks
        // AST::simd_lane_op_lowers. a pair that matrix rejected is a compiler bug here,
        // not a second copy of which operators a lane allows
        const AST::Operator *op = node.op_node->op;
        const AST::ValueType lane = AST::ValueType::make_mutable(
            lhs.is_simd() ? lhs.simd_element() : rhs.simd_element());
        const bool lowers = AST::simd_lane_op_lowers(op->type, lane);

        if (op->is_shift()) {
            if (!lhs.is_simd() || rhs.is_simd() || !lowers
                || !rhs.is_integer_type() || rhs.is_wrapped_optional()) {
                throw unlowered();
            }
        } else if (!lhs.is_simd() || !rhs.is_simd()
            || AST::ValueType::make_mutable(lhs) != AST::ValueType::make_mutable(rhs)
            || !lowers) {
            throw unlowered();
        }

        const bool is_unsigned = lane.is_unsigned_integer();
        const bool is_float = lane.is_floating_type();

        auto splat_count = [&]() {
            llvm::Value *count = _ctx.types->coerce_value(
                CodegenValue::scalar(right), AST::value_type_of(rhs), lane, *_ctx.current_cmp_unit).scalar();
            auto *vec_ty = llvm::cast<llvm::FixedVectorType>(left->getType());

            return splat(count, vec_ty);
        };

        switch (node.op_node->op->type) {
            case Token::Type::t_op_add:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFAdd(left, right)
                    : _ctx.builder->CreateAdd(left, right));
                return;
            case Token::Type::t_op_sub:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFSub(left, right)
                    : _ctx.builder->CreateSub(left, right));
                return;
            case Token::Type::t_op_mul:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFMul(left, right)
                    : _ctx.builder->CreateMul(left, right));
                return;
            case Token::Type::t_op_div:
                _ctx.push_scalar(_ctx.builder->CreateFDiv(left, right));
                return;
            case Token::Type::t_and:
                _ctx.push_scalar(_ctx.builder->CreateAnd(left, right));
                return;
            case Token::Type::t_or:
                _ctx.push_scalar(_ctx.builder->CreateOr(left, right));
                return;
            case Token::Type::t_xor:
                _ctx.push_scalar(_ctx.builder->CreateXor(left, right));
                return;
            case Token::Type::t_op_shl:
                _ctx.push_scalar(_ctx.builder->CreateShl(left, splat_count()));
                return;
            case Token::Type::t_op_shr:
                _ctx.push_scalar(is_unsigned
                    ? _ctx.builder->CreateLShr(left, splat_count())
                    : _ctx.builder->CreateAShr(left, splat_count()));
                return;
            case Token::Type::t_logical_eq:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFCmpOEQ(left, right)
                    : _ctx.builder->CreateICmpEQ(left, right));
                return;
            case Token::Type::t_logical_neq:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFCmpONE(left, right)
                    : _ctx.builder->CreateICmpNE(left, right));
                return;
            case Token::Type::t_close_angle:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFCmpOGT(left, right)
                    : is_unsigned
                        ? _ctx.builder->CreateICmpUGT(left, right)
                        : _ctx.builder->CreateICmpSGT(left, right));
                return;
            case Token::Type::t_open_angle:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFCmpOLT(left, right)
                    : is_unsigned
                        ? _ctx.builder->CreateICmpULT(left, right)
                        : _ctx.builder->CreateICmpSLT(left, right));
                return;
            case Token::Type::t_logical_geq:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFCmpOGE(left, right)
                    : is_unsigned
                        ? _ctx.builder->CreateICmpUGE(left, right)
                        : _ctx.builder->CreateICmpSGE(left, right));
                return;
            case Token::Type::t_logical_leq:
                _ctx.push_scalar(is_float
                    ? _ctx.builder->CreateFCmpOLE(left, right)
                    : is_unsigned
                        ? _ctx.builder->CreateICmpULE(left, right)
                        : _ctx.builder->CreateICmpSLE(left, right));
                return;
            default:
                throw unlowered();
        }
    }

    void SimdCodegen::gen_unary(AST::UnaryExprNode &node, const AST::ValueType &type, llvm::Value *value)
    {
        const AST::ValueType lane = AST::ValueType::make_mutable(type.simd_element());

        // AST::simd_lane_op_lowers is what unary_has_builtin_meaning asked. arriving here
        // with a lane it rejected is the same compiler bug gen_binary reports
        if (!AST::simd_lane_op_lowers(node.token_operator.type(), lane)) {
            throw _ctx.error(fmt::format(
                "compiler bug: unary '{}' has no lowering for '{}', but "
                "AST::unary_has_builtin_meaning accepted it {}",
                node.token_operator.value(), type.get_type_desciption(),
                _ctx.function_context()));
        }

        switch (node.token_operator.type()) {
            case Token::Type::t_op_sub:
                _ctx.push_scalar(lane.is_floating_type()
                    ? _ctx.builder->CreateFNeg(value)
                    : _ctx.builder->CreateNeg(value));
                return;

            case Token::Type::t_tilde:
                _ctx.push_scalar(_ctx.builder->CreateNot(value, "bitnot"));
                return;

            case Token::Type::t_exclamation:
                _ctx.push_scalar(_ctx.builder->CreateNot(value, "not"));
                return;

            default:
                throw _ctx.error(fmt::format(
                    "compiler bug: unary '{}' has no lowering for '{}' {}",
                    node.token_operator.value(), type.get_type_desciption(),
                    _ctx.function_context()));
        }
    }

    llvm::Value *SimdCodegen::eval_arg(AST::FunctionCallExprNode &node, size_t index)
    {
        if (index >= node.arguments.size() || node.arguments[index] == nullptr) {
            throw _ctx.error(fmt::format(
                "'{}' is missing argument {} {}",
                node.decl->builtin.value(), index, _ctx.function_context()));
        }

        node.arguments[index]->accept(*_ctx.visitor);

        return _ctx.pop_scalar();
    }

    unsigned SimdCodegen::bound_n(AST::FunctionCallExprNode &node, size_t arg_index)
    {
        if (arg_index >= node.decl->instantiation_args.size()) {
            throw _ctx.error(fmt::format(
                "Builtin '{}' is missing a lane count {}",
                node.decl->builtin.value(), _ctx.function_context()));
        }

        const AST::ValueType &length = node.decl->instantiation_args[arg_index];

        if (!length.is_const_value()) {
            throw _ctx.error(fmt::format(
                "Builtin '{}' has an unbound lane count {}",
                node.decl->builtin.value(), _ctx.function_context()));
        }

        return static_cast<unsigned>(length.const_value_bits());
    }

    void SimdCodegen::gen_simd_builtin(
        AST::FunctionCallExprNode &node,
        AST::BuiltinKind kind
    )
    {
        llvm::Type *result_ty = _ctx.types->get_llvm_type(
            node.decl->get_return_type(), *_ctx.current_cmp_unit);

        switch (kind) {
            case AST::BuiltinKind::t_simd_splat: {
                auto *vec_ty = llvm::cast<llvm::FixedVectorType>(result_ty);
                llvm::Value *scalar = eval_arg(node, 0);

                if (node.decl->instantiation_args.size() >= 1) {
                    scalar = _ctx.types->coerce_value(
                        CodegenValue::scalar(scalar),
                        node.arguments[0]->result_type(),
                        node.decl->instantiation_args[0],
                        *_ctx.current_cmp_unit).scalar();
                }

                _ctx.push_scalar(splat(scalar, vec_ty));
                return;
            }

            case AST::BuiltinKind::t_simd_load: {
                auto *vec_ty = llvm::cast<llvm::FixedVectorType>(result_ty);
                llvm::Value *ptr = eval_arg(node, 0);
                llvm::LoadInst *load = _ctx.builder->CreateAlignedLoad(
                    vec_ty, ptr, packed_alignment(node.decl->get_return_type()), "simd.load");

                _ctx.push_scalar(load);
                return;
            }

            case AST::BuiltinKind::t_simd_store: {
                llvm::Value *ptr = eval_arg(node, 0);
                llvm::Value *value = eval_arg(node, 1);
                _ctx.builder->CreateAlignedStore(
                    value, ptr, packed_alignment(node.arguments[1]->result_type()));

                return;
            }

            case AST::BuiltinKind::t_simd_select: {
                llvm::Value *mask = eval_arg(node, 0);
                llvm::Value *on_true = eval_arg(node, 1);
                llvm::Value *on_false = eval_arg(node, 2);

                _ctx.push_scalar(_ctx.builder->CreateSelect(mask, on_true, on_false, "simd.select"));
                return;
            }

            case AST::BuiltinKind::t_simd_bitmask: {
                // bit i is lane i. N<=8 bitcasts the i1 vector to an integer (pmovmskb on
                // x86; both Echo targets are little-endian so element 0 is the low bit).
                // N=16 is two 8-lane `addv`s of byte powers: a 16xi32 horizontal add was
                // what the map probe actually emitted
                llvm::Value *mask = eval_arg(node, 0);
                const unsigned n = bound_n(node, 0);
                llvm::LLVMContext &ll = *_ctx.llvm_context;
                llvm::Type *i64 = llvm::Type::getInt64Ty(ll);

                // N<=8 is a bitcast to iN. the only wider mask the shape rule admits is
                // k_simd_max_bytes bool lanes (a lane is one byte). that lowering is a
                // 16-lane split; a cap that is no longer 16 has to grow a new one rather
                // than shuffle past the end of a 16-lane vector
                if (n <= 8) {
                    llvm::Type *packed = llvm::Type::getIntNTy(ll, n);
                    llvm::Value *bits = _ctx.builder->CreateBitCast(
                        mask, packed, "simd.bitmask.bits");

                    _ctx.push_scalar(_ctx.builder->CreateZExt(bits, i64, "simd.bitmask"));
                    return;
                }

                if (n != AST::k_simd_max_bytes) {
                    throw _ctx.error(fmt::format(
                        "compiler bug: simd::bitmask has no lowering for {} lanes {}",
                        n, _ctx.function_context()));
                }

                static_assert(AST::k_simd_max_bytes == 16,
                    "simd::bitmask's wide path splits 16 lanes; a wider cap needs a new lowering");

                llvm::Type *i8 = llvm::Type::getInt8Ty(ll);
                auto *bytes_ty = llvm::FixedVectorType::get(i8, n);
                // sext, not zext: AND with 0x80 must keep the high power (1 & 0x80 is 0)
                llvm::Value *bytes = _ctx.builder->CreateSExt(
                    mask, bytes_ty, "simd.mask.sext");

                llvm::SmallVector<llvm::Constant *, 16> powers;

                for (unsigned i = 0; i < n; i++) {
                    powers.push_back(llvm::ConstantInt::get(i8, 1u << (i % 8)));
                }

                llvm::Value *weighted = _ctx.builder->CreateAnd(
                    bytes, llvm::ConstantVector::get(powers), "simd.mask.bits");

                auto *half_ty = llvm::FixedVectorType::get(i8, 8);
                llvm::Value *poison = llvm::PoisonValue::get(bytes_ty);
                llvm::SmallVector<int, 8> lo_idx = { 0, 1, 2, 3, 4, 5, 6, 7 };
                llvm::SmallVector<int, 8> hi_idx = { 8, 9, 10, 11, 12, 13, 14, 15 };
                llvm::Value *lo = _ctx.builder->CreateShuffleVector(
                    weighted, poison, lo_idx, "simd.mask.lo");
                llvm::Value *hi = _ctx.builder->CreateShuffleVector(
                    weighted, poison, hi_idx, "simd.mask.hi");

                llvm::FunctionType *reduce_ty = llvm::FunctionType::get(i8, { half_ty }, false);
                std::string failure;
                llvm::Function *reduce = declare_intrinsic(
                    _ctx.current_module(), "llvm.vector.reduce.add", reduce_ty, failure);

                if (reduce == nullptr) {
                    throw _ctx.error(fmt::format(
                        "cannot lower simd::bitmask: {}", failure));
                }

                llvm::Value *sum_lo = _ctx.builder->CreateCall(
                    reduce, { lo }, "simd.bitmask.lo");
                llvm::Value *sum_hi = _ctx.builder->CreateCall(
                    reduce, { hi }, "simd.bitmask.hi");
                llvm::Value *w_lo = _ctx.builder->CreateZExt(
                    sum_lo, i64, "simd.bitmask.lo64");
                llvm::Value *w_hi = _ctx.builder->CreateZExt(
                    sum_hi, i64, "simd.bitmask.hi64");
                llvm::Value *shifted = _ctx.builder->CreateShl(
                    w_hi, llvm::ConstantInt::get(i64, 8), "simd.bitmask.shift");

                _ctx.push_scalar(_ctx.builder->CreateOr(w_lo, shifted, "simd.bitmask"));
                return;
            }

            case AST::BuiltinKind::t_simd_from_array: {
                auto *vec_ty = llvm::cast<llvm::FixedVectorType>(result_ty);
                llvm::Value *arr = eval_arg(node, 0);
                llvm::Value *vec = llvm::PoisonValue::get(vec_ty);
                const unsigned n = vec_ty->getNumElements();

                for (unsigned i = 0; i < n; i++) {
                    llvm::Value *lane = _ctx.builder->CreateExtractValue(arr, { i });
                    vec = _ctx.builder->CreateInsertElement(vec, lane, i);
                }

                _ctx.push_scalar(vec);
                return;
            }

            case AST::BuiltinKind::t_simd_to_array: {
                auto *arr_ty = llvm::cast<llvm::ArrayType>(result_ty);
                llvm::Value *vec = eval_arg(node, 0);
                llvm::Value *arr = llvm::PoisonValue::get(arr_ty);
                const unsigned n = static_cast<unsigned>(arr_ty->getNumElements());

                for (unsigned i = 0; i < n; i++) {
                    llvm::Value *lane = _ctx.builder->CreateExtractElement(vec, i);
                    arr = _ctx.builder->CreateInsertValue(arr, lane, { i });
                }

                _ctx.push_scalar(arr);
                return;
            }

            default:
                throw _ctx.error(fmt::format(
                    "Builtin '{}' is not a simd verb {}",
                    node.decl->builtin.value(), _ctx.function_context()));
        }
    }
};
