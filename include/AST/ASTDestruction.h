#ifndef ASTDESTRUCTION_H
#define ASTDESTRUCTION_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    // does a value of this type have to be destroyed when its owner goes out of scope?
    // true if the type declares a destructor, or any property needs one. a class and a
    // `weak<T>` always answer true (a release / a weak-count drop). a pointer is not an
    // owner. a layout cycle answers false so this walk does not recurse while TypeChecker
    // still reports. not cached: `Box<Buffer>` and `Box<int32>` share one template
    bool needs_destruction(const ValueType &type);

    // does a value of this layout owe a teardown (a class handle owes a release and asks later)?
    // ensure_deinit is the one function that tears the value down
    bool needs_deinit(const ComplexType *type);

    // does that teardown have to reach inside, or is a declared destructor the whole of it?
    // reaching inside has to happen in a body the type owns: a `private` property refuses any other
    bool properties_need_destruction(const ComplexType *type);
};

#endif
