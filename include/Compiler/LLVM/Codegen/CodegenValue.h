#ifndef CODEGENVALUE_H
#define CODEGENVALUE_H

#pragma once

#include "AST/ASTValueType.h"

#include <llvm/Support/ErrorHandling.h>

namespace llvm
{
    class Type;
    class Value;
};

namespace Compiler::LLVM
{
    // **where an address came from, which decides whether an access through it may be tagged.**
    //
    // a *typed* place was reached without ever leaving the compiler's own accounting: a local, a
    // field of one, an element a container handed back. its storage is the type it says it is,
    // because the only way to make that false is a reinterpretation, and that now needs `unsafe`.
    //
    // a *raw* place went through a `ptr<T>` - the pointer that a reinterpretation produces, the one
    // `mem::copy` walks bytes through, the one an FFI call hands back. an access through it is
    // emitted with no `!tbaa` at all, and an untagged instruction may alias anything: the
    // conservative answer, said by omission
    //
    // an *overlapping* place is an enum payload field (or a member reached through one). the bytes
    // are typed for the live case, but another case's field sits at the same offset, so a per-type
    // `!tbaa` tag would be C's union lie. omission again, and not `t_raw`: the address never left
    // the compiler's accounting
    enum class Provenance
    {
        t_typed,
        t_raw,
        t_overlapping,
    };

    // an addressable location: where the storage lives, and what it holds
    //
    // the type has to travel with the address. under llvm's opaque pointers every pointer is
    // the same `ptr`, so an llvm::Value alone says nothing about what it points at - and every
    // load needs its element type spelled out
    struct LValue
    {
        llvm::Value *address = nullptr;

        // st(E) in the model: the type of the thing *at* `address`, before any auto-deref
        // for `ptr<int32> $p` this is ptr<int32>, and the address is $p's own slot
        AST::ValueType storage_type;

        // **it travels with the address for storage_type's reason.** by the time a load is emitted
        // the expression is long gone, and "was there a raw pointer anywhere on the way here" is not
        // a question an `llvm::Value *` can be asked
        Provenance provenance = Provenance::t_typed;
    };

    // an r-value: either an SSA scalar, or a large aggregate that lives at an address.
    // Clang's AggValueSlot. a copy of the second kind is memcpy, never a load of the whole type.
    // this is the only value on the codegen stack and at load/store/coerce/return/optional
    struct CodegenValue
    {
        enum class Kind
        {
            t_scalar,
            t_aggregate,
        };

        Kind kind = Kind::t_scalar;
        llvm::Value *value = nullptr;
        llvm::Type *aggregate_type = nullptr;
        Provenance provenance = Provenance::t_typed;

        static CodegenValue scalar(llvm::Value *ssa)
        {
            CodegenValue result;
            result.kind = Kind::t_scalar;
            result.value = ssa;
            return result;
        }

        static CodegenValue aggregate(llvm::Value *address, llvm::Type *type, Provenance provenance)
        {
            CodegenValue result;
            result.kind = Kind::t_aggregate;
            result.value = address;
            result.aggregate_type = type;
            result.provenance = provenance;
            return result;
        }

        bool is_aggregate() const {
            return kind == Kind::t_aggregate;
        }

        llvm::Value *scalar() const
        {
            if (kind != Kind::t_scalar || value == nullptr) {
                llvm::report_fatal_error("a memory aggregate was used as an SSA value");
            }

            return value;
        }
    };
};

#endif
