#ifndef ASTMUTATION_H
#define ASTMUTATION_H

#pragma once

namespace AST
{
    class Collector;
    class Node;
    class VarDeclNode;

    // can nothing in `subtree` write through `decl`, so foreach / match may elide the copy to
    // `const V&`? a wrong true is ConstViolation (or a match arm writing the original place).
    // a proof: it does not write the tree, and it must not score the call against `$el` (that
    // stalls on the binding type this is deciding). the overload *set* is CallResolver's.
    // `echo` neither writes nor borrows. RecursiveVisitor so a new write-shaped node does not compile
    bool is_never_written(const VarDeclNode &decl, Node &subtree, Collector &collector);
};

#endif
