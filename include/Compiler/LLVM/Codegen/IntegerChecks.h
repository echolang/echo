#ifndef INTEGERCHECKS_H
#define INTEGERCHECKS_H

#pragma once

#include "Token.h"

#include <llvm/IR/Value.h>

namespace Compiler::LLVM
{
    struct CodegenContext;

    // **integer ops LLVM leaves undefined, checked in a debug build.**
    // `/` `%` by zero and `MIN / -1` are UB; a shift past the width is poison. release pays nothing.
    // emitted only when assertions_enabled(), so release IR is unchanged by construction

    // `/` or `%`: divisor zero, or signed `dividend == MIN && divisor == -1`
    void gen_division_check(
        CodegenContext &ctx,
        llvm::Value *dividend,
        llvm::Value *divisor,
        bool is_unsigned,
        const TokenReference &at
    );

    // `<<` or `>>`: `count` unsigned-compared against `shifted_width`, so a negative count is the
    // same miss. the count is the original, before it is narrowed to the shifted type: a uint32 259
    // on a uint8 shift is 259 >= 8, not 3 >= 8 after truncation
    void gen_shift_count_check(
        CodegenContext &ctx,
        llvm::Value *count,
        unsigned shifted_width,
        const TokenReference &at
    );

    // integer `**`: exponentiation by squaring. a negative exponent answers 0. overflow
    // traps in a debug build and wraps in release, same gate as the two checks above
    llvm::Value *gen_integer_pow(
        CodegenContext &ctx,
        llvm::Value *base,
        llvm::Value *exponent,
        bool is_unsigned,
        const TokenReference &at
    );
};

#endif
