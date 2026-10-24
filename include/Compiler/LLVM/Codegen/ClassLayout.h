#ifndef CLASSLAYOUT_H
#define CLASSLAYOUT_H

#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>

namespace Compiler::LLVM
{
    // heap block a class lives in. the payload is wrapped, so member GEPs are the struct path:
    //
    //     %Foo.box = { i64 __strong, i64 __weak, ptr __typeinfo, %Foo }
    //
    // __strong == 0 runs deinit; __weak == 0 frees the block. the strong refs hold one weak,
    // so the only `free` in the runtime is `__eco_weak_release`
    namespace ClassBox
    {
        static constexpr unsigned strong_index = 0;
        static constexpr unsigned weak_index = 1;
        static constexpr unsigned typeinfo_index = 2;
        static constexpr unsigned payload_index = 3;

        // **the strong count while teardown runs.** parked here so a destructor that retains `$this`
        // cannot take 0 -> 1 -> 0 and re-enter. negative, far below the `-0xEAD` poison. weak upgrade tests `<= 0`
        static constexpr long long deiniting_count = -(1LL << 62);
    };

    // **a wrapped `T?` has no entry here, and that is the point.** its two slots are `AST::ComplexType`
    // properties - `AST::k_optional_has_index` / `k_optional_value_index` - because a tagged optional is an
    // interned layout rather than a shape codegen invents. ClassBox exists precisely because the block
    // around a class payload has *no* AST counterpart to read the order off; an optional does, and a
    // second copy of it here would be the property order and the GEP order agreeing only by hand

    // what the typeinfo global *holds*.
    //
    // It began as one `i8 0` whose address was the whole of a class's identity, and that stays true.
    // `$obj instanceof Circle` is still one address comparison, and the struct below is
    // source-compatible with it for exactly that reason. What it grew for is the second question a class
    // can now be asked: which **interfaces** does it conform to
    //
    //     %eco.typeinfo = { i64 conformance_count, ptr conformances }
    //
    // `conformances` points at a `[N x { ptr identity, ptr vtable }]`. identities are known
    // during struct maps; vtables are filled after function maps, because a vtable names
    // method bodies that do not exist yet. null when the count is zero
    //
    // **dispatch still uses the value's own vtable**, filled at the widening, so a call is one load.
    // a recast is a scan of this table: match identity, seat the neighbour vtable
    namespace ClassTypeInfo
    {
        static constexpr unsigned conformance_count_index = 0;
        static constexpr unsigned conformances_index = 1;

        // appended so gen_conformance_scan's two GEPs stay on 0 and 1. bit 0 of flags is
        // `#[atomic]`. deinit is the environment teardown `__eco_release_env` loads: null
        // when the payload owns nothing
        static constexpr unsigned flags_index = 2;
        static constexpr unsigned deinit_index = 3;

        static constexpr uint64_t atomic_flag = 1;
    };

    // one row of the array typeinfo.conformances points at. instanceof is "is there a row
    // whose identity matches"; recast loads the vtable beside it. two questions, one scan
    namespace ClassConformance
    {
        static constexpr unsigned identity_index = 0;
        static constexpr unsigned vtable_index = 1;
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
