#ifndef ASTCLASSLOCALS_H
#define ASTCLASSLOCALS_H

#pragma once

namespace AST
{
    class Collector;
    class Module;

    // **was this class handle assigned before it was read?** a class handle's zero is null, so
    // `$n->tag` through an uninitialised local would go through it. whole-variable assignment
    // seats it. joining uses ConstructionWalk's falls_through (`scope_always_exits`): a `break`
    // does not poison the successor. a `ReleaseNode` is a null-handle no-op, not a use.
    //
    // asked of a module after TypeChecker has walked it, so every declaration already has a type.
    // a sibling of `check_construction`, not a second control-flow interpreter inside TypeChecker
    void check_class_locals(Module &module, Collector &collector);
};

#endif
