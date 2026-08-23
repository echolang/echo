#ifndef COUNTACCESS_H
#define COUNTACCESS_H

#pragma once

#include "Compiler/LLVM/Codegen/CountAtomics.h"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace AST
{
    class ComplexType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // typed retain/release bake t_plain / t_atomic; an environment has no ComplexType
    // and is minted atomic. one answer so the five call sites cannot drift
    CountAccess count_access_for(const AST::ComplexType *complex);

    // bit 0 of the typeinfo flags word. the erased retain and the weak release are one body
    // for every class, so they load this rather than baking a protocol in
    llvm::Value *counts_are_atomic_flag(
        CodegenContext &ctx,
        llvm::Value *handle,
        llvm::Type *box_type
    );

    // run `emit(atomic)` once, or twice behind the typeinfo flag with a phi.
    // increment ignores the return; decrement needs the next count
    llvm::Value *apply_count_access(
        CodegenContext &ctx,
        CountAccess access,
        llvm::Value *handle,
        llvm::Type *box_type,
        const char *label,
        llvm::function_ref<llvm::Value *(bool atomic)> emit
    );
};

#endif
