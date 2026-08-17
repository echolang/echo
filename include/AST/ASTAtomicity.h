#ifndef ASTATOMICITY_H
#define ASTATOMICITY_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    // is this class's reference count an atomic RMW?
    //
    // `#[atomic]` on the concrete class, carried across derive_instantiation beside `is_unique`.
    // asked only of a ComplexType: there is no "unknown, so yes" arm. that arm mixed an atomic
    // increment with a plain decrement on one word, which is a data race rather than a conservative
    // default. an erased retain or a weak release that cannot see the class loads the same fact
    // from the typeinfo flags word and branches
    //
    // a closure environment answers true because TypeRegistry::create_anonymous_type sets the flag
    // when it mints a class - a spawned environment is shared by construction, and there is no
    // written attribute on an anonymous type
    inline bool counts_are_atomic(const ComplexType &type) {
        return type.is_atomic;
    }
};

#endif
