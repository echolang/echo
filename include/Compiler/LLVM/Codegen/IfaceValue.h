#ifndef IFACEVALUE_H
#define IFACEVALUE_H

#pragma once

namespace Compiler::LLVM
{
    // an interface value: `{ ptr object, ptr vtable }`. object is field 0 so `&iface.object` is the
    // `$this` a class method already expects. the vtable is resolved at the widening
    namespace IfaceValue
    {
        static constexpr unsigned object_index = 0;
        static constexpr unsigned vtable_index = 1;

        // **slot 0 of every vtable is the implementor's release thunk**, and the methods start after it.
        //
        // an erased value owns a reference and has to be able to give it back, but the release site only
        // knows the *interface* - `Drawable` says nothing about which block to free or which deinit to
        // run. so the answer has to be reachable from the value, and the vtable is already the thing this
        // design resolves at the widening precisely so nothing has to be searched for later. one pointer
        // per (class, interface) pair is the whole cost, and the release is then one load like a call.
        //
        // the alternative was a release slot in the *typeinfo* descriptor, which is what a generic closure wants
        // for a closure environment. that stays the right home for A27 and is deliberately not what this
        // uses: the descriptor is built during type lowering, before any function body exists, so filling
        // it would need the thunk to be created before the layout it is built from
        static constexpr unsigned vtable_release_slot = 0;
        static constexpr unsigned first_method_slot = 1;
    };
};

#endif
