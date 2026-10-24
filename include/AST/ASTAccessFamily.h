#ifndef ASTACCESSFAMILY_H
#define ASTACCESSFAMILY_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    // which accesses may alias: the language table `!tbaa` is generated from.
    // coarser than the types, wherever two spellings can name one live object:
    //   - `int8` / `uint8` are byte, the ancestor of every typed family
    //   - signed and unsigned of one width share a family
    //   - `usize` / `isize` answer the target-width integer
    //   - every address is one family
    //   - an aggregate answers `t_none` (emit no metadata)
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
