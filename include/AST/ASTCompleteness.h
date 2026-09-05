#ifndef ASTCOMPLETENESS_H
#define ASTCOMPLETENESS_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    // **does a value of this type have a known size, alignment and copy?**
    //
    // three answers rather than a bool, for const_fold's reason: a not-yet must not read as a
    // refusal. collapsing pending into incomplete made `size_of<T>()` in a template name the
    // parameter as a type that will never have a layout, instead of waiting until T is bound
    //
    // asked of a ValueType, not a ComplexType, so `ptr<Handle>` is complete (one word) while
    // `Handle` is not. a caller that cares about a pointee asks about the pointee
    //
    // the sole owner of the size question. "the target is not in yet" is a different one -
    // AST::builtin_foldability's `t_needs_layout`, which size_of still asks when the DataLayout
    // has not arrived. AST::read_peels_pointer is the auto-deref question, which asks this of
    // the pointee; TypeChecker, size_of, classify_copy and pointer arithmetic read this rather
    // than re-deriving "is this an opaque type" at the use
    enum class TypeCompleteness
    {
        // a value of this type has a known size, alignment and copy
        t_complete,

        // named, and never will - `extern struct`
        t_incomplete,

        // unsubstituted type parameter, or anything still unknown
        t_pending,
    };

    TypeCompleteness type_completeness(const ValueType &type);

    // **why may this type not appear on a declaration?** nullopt when it may.
    //
    // a `ptr<Handle>` is a value; a `Handle` is not; a `Handle&` is a borrow of storage
    // Echo does not account for. asked once by AST::TypeChecker of every declaration, the same
    // sweep `c_function_type_refusal` already is - a local, a parameter, a return and a property
    // refused by the same sentence
    std::optional<std::string> incomplete_use_refusal(const ValueType &type);

    // **why may this pointer not be offset or indexed?** nullopt when it may.
    //
    // asked of the pointer type, about its pointee. `$p:$ + 1` and `$p:$[i]` both need a stride,
    // and the sentence is the same one
    std::optional<std::string> incomplete_stride_refusal(const ValueType &pointer);
};

#endif
