#ifndef CLASSLAYOUT_H
#define CLASSLAYOUT_H

#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>

namespace Compiler::LLVM
{
    // the heap block a class instance lives in. one llvm::StructType per class per compilation unit,
    // wrapping the *payload* - which is the exact layout a `struct` with the same body would have had,
    // built by the same code. that nesting is the whole trick: below the payload every member GEP is
    // the struct path unchanged, so classes needed no second member-access implementation
    //
    //     %Foo.box = { i64 __strong, ptr __typeinfo, %Foo }
    //
    // the strong count is at offset 0 so retain and release reach it without an offset. __typeinfo
    // holds the address of the class's own `@Foo.typeinfo` global, which is what `instanceof` compares
    // - an address is exact identity across modules and needs no numbering scheme to keep stable
    //
    // a class-typed value is *not* this type: it is an opaque `ptr` to a block of it. the layout is
    // only ever needed to size an allocation, to reach the payload, and to touch the count
    namespace ClassBox
    {
        static constexpr unsigned strong_index = 0;
        static constexpr unsigned typeinfo_index = 1;
        static constexpr unsigned payload_index = 2;
    };

    // what the typeinfo global *holds*. it began as one `i8 0` whose address was the whole of a class's
    // identity, and that stays true - `$obj instanceof Circle` is still one address comparison, and the
    // struct below is source-compatible with it for exactly that reason. what it grew for is the second
    // question a class can now be asked: which **interfaces** does it conform to
    //
    //     %eco.typeinfo = { i64 conformance_count, ptr conformances }
    //
    // `conformances` points at a `[N x ptr]` of interface identities - `@Drawable.itype` and friends,
    // each its own `linkonce_odr` byte whose address is that interface's identity, exactly as a class's
    // typeinfo address is the class's. null when the count is zero, which is every class today that
    // declares no conformance and every closure environment
    //
    // **the vtable is deliberately not in here.** dispatch resolves its vtable at the *widening* site,
    // where the concrete class is statically known, so an interface value carries it directly and a call
    // costs one load rather than a scan. that leaves each structure answering exactly one question: this
    // table answers "is it one", the value's own vtable pointer answers "which method"
    namespace ClassTypeInfo
    {
        static constexpr unsigned conformance_count_index = 0;
        static constexpr unsigned conformances_index = 1;
    };

    // what a class needs from codegen beyond the handle. resolved together because the box cannot be
    // built without the payload and the typeinfo global has the same lifetime as both
    struct ClassLayout
    {
        llvm::StructType *payload = nullptr;
        llvm::StructType *box = nullptr;
        llvm::GlobalVariable *typeinfo = nullptr;
    };
};

#endif
