#ifndef RETURNABI_H
#define RETURNABI_H

#pragma once

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace Compiler::LLVM
{
    // **how does a function hand its answer back?** one question, one owner, and the reason it needs an
    // owner rather than a line at the signature is that *four* places have to agree byte for byte or the
    // program miscompiles in silence: the signature TypeLowering builds, the prologue that names the
    // arguments, the `return` that fills it in, and every call site that reads it. A caller that thinks a
    // struct comes back in registers while the callee wrote it through a pointer reads whatever was in
    // those registers.
    //
    // **why it is not simply "return the struct".** LLVM lets a function return a first-class aggregate and
    // returning the struct as a first-class aggregate is correct and slow: a function with three `return`s of a struct
    // becomes a phi of that struct, LLVM's if-conversion refuses to speculate a phi of an aggregate, and a
    // loop calling it keeps a branch per return - so LoopVectorize cannot widen it. Measured at 4.7x against
    // the same program in Rust, and 1.00x once the aggregate is out of the signature. Both clang and rustc
    // do this, and for this reason.
    //
    // it is deliberately **not** the full C ABI. Echo emits both halves of every call it makes, so any
    // consistent convention is a correct one; what it must not do is *claim* to match C for a type where it
    // does not - a float aggregate goes back in vector registers on AArch64 and this says nothing about
    // that. so `extern` declarations keep the shape they have always had, and the one place that is still
    // wrong is still wrong. see the note in TypeLowering::get_function_type
    // there are two answers and the aggregate is the whole of what tells them apart, so it is the
    // whole of what is stored. **direct** is the value as written - every scalar, every pointer, and
    // every aggregate small enough that LLVM's own lowering puts it in registers. **indirect** is the
    // caller passing storage as a hidden *first* argument for a function that then returns `void`, the
    // classic `sret`, and what takes the aggregate out of the signature.
    //
    // a kind beside the type would be two fields owing each other an invariant no reader states, and
    // every reader already asks is_indirect() and then reads the type it points at
    struct ReturnAbi
    {
        // the aggregate the hidden argument points at. null when the answer comes back directly
        llvm::Type *indirect_type = nullptr;

        bool is_indirect() const {
            return indirect_type != nullptr;
        }
    };

    // **two registers' worth is the line**, which is the same one AArch64 and x86-64 both draw and is why
    // it is a size and not a field count: what decides is whether the backend can hand the thing back in
    // registers at all, and below the line it already does. a `{ i8, i64, i32 }` - which is what a
    // `result<int64, int32>` lowers to - is 24 bytes and goes indirect; a `string` view pair does not.
    //
    // asked of the **lowered** type rather than the ValueType, because the answer is about machine
    // registers and only the DataLayout knows the size. a `void` return and an opaque pointer both answer
    // direct without a special case, neither being an aggregate
    inline ReturnAbi return_abi_for(llvm::Type *lowered_return, const llvm::DataLayout &layout)
    {
        if (lowered_return == nullptr || !lowered_return->isAggregateType()) {
            return ReturnAbi{};
        }

        // **sized, and asked before the size is.** an unsized aggregate has no answer to give and reaching
        // for one asserts inside LLVM, so it keeps the shape it had rather than becoming a pointer to
        // something whose size nothing knows
        if (!lowered_return->isSized()) {
            return ReturnAbi{};
        }

        if (layout.getTypeAllocSize(lowered_return) <= 16) {
            return ReturnAbi{};
        }

        return ReturnAbi{ lowered_return };
    }

    // **the attributes argument zero carries, and they go on the *call* as well as on the function.**
    // that is not belt and braces - it is the whole of what decides which register the hidden pointer
    // travels in, and LLVM lets the two disagree in silence.
    //
    // a call whose argument zero is not marked `sret` is lowered as an ordinary pointer argument, so on
    // AArch64 it goes in x0 and every real argument shifts one register up; a function whose parameter
    // zero *is* marked reads it from x8. LLVM papers over a missing call-site attribute by falling back
    // to `CallBase::getCalledFunction()`'s attributes - and that lookup answers **null** when the call's
    // FunctionType is not the callee's. Which is exactly what a merged module produces: `llvm::Linker`
    // brings in each unit's own named struct types, so `string` becomes `%string` and `%string.3`, two
    // types of one shape and two identities, and a cross-unit call site's type stops matching its
    // callee's. The attribute is dropped, the arguments shift, and a `string`'s size field arrives
    // holding somebody's pointer - a memmove of a garbage length, faulting nowhere near the cause.
    //
    // so nothing may rely on the inheritance. one builder, applied to both, and neither site spells it
    inline llvm::AttrBuilder indirect_return_attributes(
        llvm::LLVMContext &context,
        const ReturnAbi &abi,
        const llvm::DataLayout &layout
    )
    {
        llvm::AttrBuilder attributes(context);

        attributes.addStructRetAttr(abi.indirect_type);

        // `noalias` says what the C ABI says: nobody else names this storage. without it the pointer
        // could alias anything and could escape, so SROA leaves the caller's slot in memory and the
        // scalars the loop needed to if-convert never appear
        attributes.addAttribute(llvm::Attribute::NoAlias);
        attributes.addAlignmentAttr(layout.getABITypeAlign(abi.indirect_type));

        return attributes;
    }
};

#endif
