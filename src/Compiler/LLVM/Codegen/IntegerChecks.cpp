#include "Compiler/LLVM/Codegen/IntegerChecks.h"

#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <algorithm>

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

};
