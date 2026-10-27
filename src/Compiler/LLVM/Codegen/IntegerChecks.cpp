#include "Compiler/LLVM/Codegen/IntegerChecks.h"

#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <string>

namespace Compiler::LLVM
{

void gen_division_check(
    CodegenContext &ctx,
    llvm::Value *dividend,
    llvm::Value *divisor,
    bool is_unsigned,
    const TokenReference &at
)
{
    if (!ctx.options.assertions_enabled()) {
        return;
    }

    llvm::Type *type = divisor->getType();

    ctx.abort->gen_abort_if(
        ctx.builder->CreateICmpEQ(divisor, llvm::ConstantInt::get(type, 0), "div.by_zero"),
        "fatal error",
        "division by zero",
        at);

    if (is_unsigned) {
        return;
    }

    const unsigned width = type->getIntegerBitWidth();
    llvm::Value *is_min = ctx.builder->CreateICmpEQ(
        dividend, llvm::ConstantInt::get(type, llvm::APInt::getSignedMinValue(width)), "div.min");
    llvm::Value *is_minus_one = ctx.builder->CreateICmpEQ(
        divisor, llvm::ConstantInt::getSigned(type, -1), "div.minus_one");

    ctx.abort->gen_abort_if(
        ctx.builder->CreateAnd(is_min, is_minus_one, "div.overflows"),
        "fatal error",
        "integer overflow: the smallest value divided by -1 has no result of its type",
        at);
}

void gen_shift_count_check(
    CodegenContext &ctx,
    llvm::Value *count,
    unsigned shifted_width,
    const TokenReference &at
)
{
    if (!ctx.options.assertions_enabled()) {
        return;
    }

    llvm::Type *count_ty = count->getType();
    const unsigned count_width = count_ty->getIntegerBitWidth();
    const unsigned wide = std::max(count_width, 64u);
    llvm::IntegerType *wide_ty = llvm::Type::getIntNTy(*ctx.llvm_context, wide);
    llvm::Value *wide_count = count_width < wide
        ? ctx.builder->CreateZExt(count, wide_ty, "shift.count")
        : count;

    ctx.abort->gen_abort_if(
        ctx.builder->CreateICmpUGE(
            wide_count, llvm::ConstantInt::get(wide_ty, shifted_width), "shift.too_far"),
        "fatal error",
        "shift count is negative or not less than the width of the value shifted",
        at);
}

llvm::Value *gen_integer_pow(
    CodegenContext &ctx,
    llvm::Value *base,
    llvm::Value *exponent,
    bool is_unsigned,
    const TokenReference &at
)
{
    llvm::Type *int_type = base->getType();
    const llvm::Align align = ctx.layout().getABITypeAlign(int_type);
    llvm::Function *function = ctx.builder->GetInsertBlock()->getParent();

    llvm::AllocaInst *result_slot = ctx.entry_alloca(int_type, "pow.result");
    llvm::AllocaInst *base_slot = ctx.entry_alloca(int_type, "pow.base");
    llvm::AllocaInst *exp_slot = ctx.entry_alloca(int_type, "pow.exp");

    auto store = [&](llvm::Value *value, llvm::Value *slot) {
        llvm::StoreInst *st = ctx.builder->CreateStore(value, slot);
        st->setAlignment(align);
    };

    auto load = [&](llvm::Value *slot, const char *name) {
        llvm::LoadInst *ld = ctx.builder->CreateLoad(int_type, slot, name);
        ld->setAlignment(align);
        return static_cast<llvm::Value *>(ld);
    };

    store(llvm::ConstantInt::get(int_type, 1), result_slot);
    store(base, base_slot);
    store(exponent, exp_slot);

    auto *loop = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.loop", function);
    auto *body = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.body", function);
    auto *mul = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.mul", function);
    auto *shift = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.shift", function);
    auto *square = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.square", function);
    auto *done = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.done", function);

    if (!is_unsigned) {
        auto *neg = llvm::BasicBlock::Create(*ctx.llvm_context, "pow.neg", function);
        llvm::Value *negative = ctx.builder->CreateICmpSLT(
            exponent, llvm::ConstantInt::get(int_type, 0), "pow.negative");
        ctx.builder->CreateCondBr(negative, neg, loop);

        ctx.set_insert_point(neg);
        store(llvm::ConstantInt::get(int_type, 0), result_slot);
        ctx.builder->CreateBr(done);
    } else {
        ctx.builder->CreateBr(loop);
    }

    ctx.set_insert_point(loop);
    llvm::Value *exp_now = load(exp_slot, "pow.exp");
    llvm::Value *zero = llvm::ConstantInt::get(int_type, 0);
    ctx.builder->CreateCondBr(
        ctx.builder->CreateICmpEQ(exp_now, zero, "pow.done"), done, body);

    ctx.set_insert_point(body);
    llvm::Value *odd = ctx.builder->CreateICmpNE(
        ctx.builder->CreateAnd(exp_now, llvm::ConstantInt::get(int_type, 1), "pow.bit"),
        zero,
        "pow.odd");
    ctx.builder->CreateCondBr(odd, mul, shift);

    const auto mul_checked = [&](llvm::Value *a, llvm::Value *b, const char *name) {
        if (!ctx.options.assertions_enabled()) {
            return ctx.builder->CreateMul(a, b, name);
        }

        const auto id = is_unsigned
            ? llvm::Intrinsic::umul_with_overflow
            : llvm::Intrinsic::smul_with_overflow;
        llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
            ctx.current_module(), id, { int_type });
        llvm::Value *pair = ctx.builder->CreateCall(intrinsic, { a, b }, name);
        llvm::Value *product = ctx.builder->CreateExtractValue(pair, 0, std::string(name) + ".v");
        llvm::Value *overflowed = ctx.builder->CreateExtractValue(pair, 1, std::string(name) + ".ov");
        ctx.abort->gen_abort_if(
            overflowed,
            "fatal error",
            "integer overflow: this '**' has no result of its type",
            at);
        return product;
    };

    ctx.set_insert_point(mul);
    store(mul_checked(load(result_slot, "pow.acc"), load(base_slot, "pow.cur"), "pow.grow"), result_slot);
    ctx.builder->CreateBr(shift);

    // shift first, then square only if bits remain. `2 ** 40` in i64 would
    // otherwise square 2^32 into 2^64 after the last multiply and trap
    ctx.set_insert_point(shift);
    store(
        ctx.builder->CreateLShr(
            load(exp_slot, "pow.exp.shr"), llvm::ConstantInt::get(int_type, 1), "pow.exp.next"),
        exp_slot);
    ctx.builder->CreateCondBr(
        ctx.builder->CreateICmpEQ(load(exp_slot, "pow.exp.left"), zero, "pow.last"),
        done,
        square);

    ctx.set_insert_point(square);
    llvm::Value *cur = load(base_slot, "pow.sq.in");
    store(mul_checked(cur, cur, "pow.sq"), base_slot);
    ctx.builder->CreateBr(loop);

    ctx.set_insert_point(done);
    return load(result_slot, "pow");
}

};
