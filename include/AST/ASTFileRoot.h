#ifndef ASTFILEROOT_H
#define ASTFILEROOT_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    class Node;
    class ScopeNode;

    // a file root is the body of `i32 main`. the parser plants `return 0` for a
    // bare file-scope `return;`, and PointerAdjuster, TypeChecker and StmtCodegen
    // all ask this, so those four cannot disagree about the type
    inline ValueType entry_return_type()
    {
        return ValueType(ValueTypePrimitive::t_int32);
    }

    // first child of a file root that is not a declaration, or null. allow-list: a NodeType added
    // without an arm here is refused in a shared file. a module-scope `$var` is storage plus an
    // initializer that runs
    Node *first_top_level_statement(const ScopeNode &root);
};

#endif
