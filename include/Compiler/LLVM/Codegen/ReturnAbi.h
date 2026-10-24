#ifndef RETURNABI_H
#define RETURNABI_H

#pragma once

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace Compiler::LLVM
{
    // how a function returns. four sites must agree (signature, prologue, `return`, call) or the
    // program miscompiles in silence.
    //
    // sized aggregates come back through sret: a first-class aggregate return becomes a phi LLVM
    // will not if-convert, so LoopVectorize cannot widen the caller. Echo emits both halves of
    // every call, so this is Echo's convention, not the C ABI; `extern` keeps the shape it already has
    struct ReturnAbi
    {
        // the aggregate the hidden argument points at. null when the answer comes back directly
        llvm::Type *indirect_type = nullptr;

        bool is_indirect() const {
            return indirect_type != nullptr;
        }
    };

    // every sized aggregate is sret. asked of the lowered LLVM type: void and pointers are direct.
    // a size cut is a trap (a packed `result<int64, E>` is 16 bytes and still an aggregate)
    inline ReturnAbi return_abi_for(llvm::Type *lowered_return)
    {
        if (lowered_return == nullptr || !lowered_return->isAggregateType()) {
            return ReturnAbi{};
        }

        // unsized aggregates have no storage to hand the caller; LLVM asserts if we ask for a size
        if (!lowered_return->isSized()) {
            return ReturnAbi{};
        }

        return ReturnAbi{ lowered_return };
    }

    // copies *inside* a function. 64 bytes is four pointers: `string`, a packed result, `Vec4`
    // stay SSA. `Vec4[512]` does not. do not fold this with return_abi_for
    constexpr uint64_t k_memory_aggregate_bytes = 64;

    inline bool aggregate_lives_in_memory(llvm::Type *type, const llvm::DataLayout &layout)
    {
        if (type == nullptr || !type->isAggregateType() || !type->isSized()) {
            return false;
        }

        return layout.getTypeAllocSize(type) > k_memory_aggregate_bytes;
    }

    // sret / noalias / align go on the call *and* the function. LLVM does not inherit them across a
    // merged module whose named structs have different identities (`%string` vs `%string.3`); a
    // missing call-site sret shifts every argument one register on AArch64
    inline llvm::AttrBuilder indirect_return_attributes(
        llvm::LLVMContext &context,
        const ReturnAbi &abi,
        const llvm::DataLayout &layout
    )
    {
        llvm::AttrBuilder attributes(context);

        attributes.addStructRetAttr(abi.indirect_type);

        // without noalias, SROA leaves the caller's slot in memory and the loop cannot if-convert
        attributes.addAttribute(llvm::Attribute::NoAlias);
        attributes.addAlignmentAttr(layout.getABITypeAlign(abi.indirect_type));

        return attributes;
    }
};

#endif
