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
