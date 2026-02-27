#ifndef IFACEVALUE_H
#define IFACEVALUE_H

#pragma once

namespace Compiler::LLVM
{
    // what an interface-typed value *is*: a fat pointer, one shape for every interface.
    //
    //     %eco.iface = { ptr object, ptr vtable }
    //
    // the same trade `eco.callable` makes, and for the same reason. the object alone knows its own type -
    // its block carries a typeinfo word - but finding a method through it would mean scanning the
    // conformance table at every call. the **widening** is where the concrete class is still statically
    // known, so that is where the vtable is resolved, and a dispatch is then one load and an indirect
    // call. the conformance table stays for `instanceof`, which is the question it answers.
    //
    // **object is field 0, and that is load-bearing.** a class method's receiver is `Circle&` - the
    // address of a slot holding a handle, not the handle itself - and `&iface.object` is exactly that
    // shape. so an erased receiver needs no shim: the address of field 0 *is* the `$this` the concrete
    // method already expects, and the same `{address, storage_type}` pair gen_lvalue hands everything
    // else works unchanged.
    //
    // a vtable slot is the requirement's ordinal in AST::interface_requirements(), which is declaration
    // order - one walk decides both what the table holds and which entry a call site reads.
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
