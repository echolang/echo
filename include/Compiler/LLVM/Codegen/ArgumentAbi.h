#ifndef ARGUMENTABI_H
#define ARGUMENTABI_H

#pragma once

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Type.h>

namespace Compiler::LLVM
{
    // how a by-value parameter travels in IR. four sites must agree (signature, prologue,
    // call, adapter) or the callee reads a different layout than the caller wrote.
    //
    // a first-class aggregate argument is a legal LLVM value and a wrong machine one: the
    // SelectionDAG backend splits it into one stack slot per field, then maps the callee's
    // local copy onto the first slot. a 4-byte `{ i16, i8, i8 }` after eight integer registers
    // arrives with only its first field intact. coercing that size to one integer of the alloc
    // size (up to 16 bytes) is the fix: an integer is not an aggregate, so copy-elision cannot
    // fire. larger aggregates stay first-class: Echo emits both halves of every call, and a
    // 40-byte `string` as a pointer plus two memcpys was 2.4x on `string_append_build`.
    // `extern` keeps the shape it already has
    struct ArgumentAbi
    {
        // the IR parameter type. null means the natural LLVM type
        llvm::Type *coerce_type = nullptr;

        bool is_direct() const {
            return coerce_type == nullptr;
        }
    };

    // one integer covers what AArch64 would have passed in two registers. a size cut on the
    // return ABI was the trap that walked a packed 16-byte result back onto the first-class
    // path - this number is that same line asked of arguments, and it is not
    // `k_memory_aggregate_bytes`
    constexpr uint64_t k_integer_argument_bytes = 16;

    inline ArgumentAbi argument_abi_for(llvm::Type *natural, const llvm::DataLayout &layout)
    {
        if (natural == nullptr || !natural->isAggregateType() || !natural->isSized()) {
            return ArgumentAbi{};
        }

        const uint64_t size = layout.getTypeAllocSize(natural);

        if (size == 0 || size > k_integer_argument_bytes) {
            return ArgumentAbi{};
        }

        ArgumentAbi abi;
        abi.coerce_type = llvm::Type::getIntNTy(natural->getContext(), static_cast<unsigned>(size * 8));
        return abi;
    }

    inline llvm::Type *argument_llvm_type(const ArgumentAbi &abi, llvm::Type *natural)
    {
        return abi.coerce_type != nullptr ? abi.coerce_type : natural;
    }
};

#endif
