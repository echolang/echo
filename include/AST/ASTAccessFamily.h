#ifndef ASTACCESSFAMILY_H
#define ASTACCESSFAMILY_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    // **which accesses may reach one another's storage: the one table.**
    //
    // it is a *language* rule before it is metadata. an `unsafe` promotion asserts, among other
    // things, that "accessing that storage as `T` is compatible with every other typed access that
    // may alias it" - and the only way that obligation can be stated to an author is if the same
    // relation the optimizer acts on is the one the language documents. so the semantic answer and
    // the `!tbaa` graph are generated from here, and there is deliberately no second switch in
    // codegen to drift from it.
    //
    // the families are coarser than the types on purpose. every place two spellings can legitimately
    // name one live object, they must share a family, or the metadata claims a separation the
    // language does not have:
    //
    //   - **`int8` and `uint8` are byte access** and alias *everything*, which is why they answer the
    //     ancestor node rather than a leaf under it. reading an object's bytes is a thing this
    //     language spells, and a sibling leaf would let a byte write be reordered past a typed one.
    //   - **a signed and an unsigned integer of one width share a family.** they are two readings of
    //     one bit pattern, and a promotion from either to the other is a thing an author may
    //     legitimately promise.
    //   - **`usize` and `isize` answer the family of their target-width integer**, so `usize` and
    //     `uint64` are one family on a 64-bit target rather than two that happen to be the same size.
    //   - **every address shares one family**, whatever it points at: what an address points at is a
    //     fact about the storage it names, not about the slot holding it, and two `ptr` slots of
    //     different pointee types are still two addresses.
    //   - **an aggregate answers nothing.** a struct by value has no honest leaf until a struct-path
    //     model is written deliberately, and `t_none` means *emit no metadata* - which is the
    //     conservative answer rather than a missing one.
    enum class AccessFamily
    {
        // no family: emit nothing, alias anything. an aggregate, a void, an unresolved type
        // parameter - and every access reached through a raw pointer, which is decided elsewhere
        t_none,

        // `int8` / `uint8`. the ancestor of every typed family rather than one beside them
        t_byte,

        t_integer16,
        t_integer32,
        t_integer64,

        t_float32,
        t_float64,

        // every pointer, borrow, class handle, weak handle, callable, interface value
        t_address,
    };

    // the family a scalar access at this type belongs to. const-blind, because `const int32` and
    // `int32` name the same bytes and a permission is not a storage type
    AccessFamily access_family_of(const ValueType &type);

    // **may an access in family `a` reach storage last written by one in `b`?**
    //
    // the relation the metadata graph encodes, said once so a diagnostic can quote it. `t_none` is
    // compatible with everything (it claims nothing) and `t_byte` is compatible with everything (it
    // is the ancestor), which is the same statement made from the two ends
    bool access_families_may_alias(AccessFamily a, AccessFamily b);

    // the node name this family contributes to the `!tbaa` graph, and the string a diagnostic uses.
    // one spelling, so the two cannot describe different graphs
    const char *access_family_name(AccessFamily family);
};

#endif
