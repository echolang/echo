#ifndef COUNTATOMICS_H
#define COUNTATOMICS_H

#pragma once

#include <llvm/IR/Instructions.h>
#include <llvm/Support/Alignment.h>

namespace Compiler::LLVM
{
    // which protocol a count bump uses. typed retain/release bake t_plain / t_atomic; an
    // environment has no ComplexType and is minted atomic; an erased retain loads the
    // typeinfo flag. CountAccess.h owns the emission
    enum class CountAccess
    {
        t_plain,
        t_atomic,
        t_from_typeinfo,
    };

    // the memory orderings of a class reference count. six pure functions of nothing: the allocation
    // counter and the static-init guard are different protocols, and a shared owner would claim they
    // participate in this one
    //
    // increment is monotonic: a retain does not publish the payload. decrement is release; the
    // matching acquire is a fence on the zero path only, first in that block, because the acquire is
    // needed only where the payload is read - one call in N. libstdc++ `_Sp_counted_base`, and A19's
    // named alternative. observe is monotonic: `mem::refs` answers how many, at some moment, and
    // licenses nothing. upgrade is a cmpxchg, not an RMW - incrementing a count already at zero
    // resurrects an object whose deinit ran
    namespace CountAtomics
    {
        inline llvm::AtomicOrdering increment() {
            return llvm::AtomicOrdering::Monotonic;
        }

        inline llvm::AtomicOrdering decrement() {
            return llvm::AtomicOrdering::Release;
        }

        inline llvm::AtomicOrdering last_reference() {
            return llvm::AtomicOrdering::Acquire;
        }

        inline llvm::AtomicOrdering observe() {
            return llvm::AtomicOrdering::Monotonic;
        }

        inline llvm::AtomicOrdering upgrade_success() {
            return llvm::AtomicOrdering::Acquire;
        }

        inline llvm::AtomicOrdering upgrade_failure() {
            return llvm::AtomicOrdering::Monotonic;
        }

        inline llvm::Align word() {
            return llvm::Align(8);
        }
    };
};

#endif
