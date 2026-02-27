#ifndef TBAATREE_H
#define TBAATREE_H

#pragma once

#include "AST/ASTValueType.h"

#include <llvm/IR/MDBuilder.h>

#include <string>
#include <unordered_map>

namespace llvm
{
    class LLVMContext;
    class MDNode;
};

namespace Compiler::LLVM
{
    // **the type-based alias tree, one per compilation unit.**
    //
    // a TBAA tag is a claim: an access at this type cannot reach storage last written at that one.
    // Aliasing against it is undefined at the IR level, so every node here is a promise the compiler
    // has to keep.
    //
    // In C that promise is a type rule, and it is famously a lie. Here it is not a type rule at all.
    // Two other things make it true instead:
    //
    //   1. **an access reached through a raw pointer is never tagged.** an untagged instruction may
    //      alias anything, which is exactly the conservative answer a `ptr<T>` deserves. It is also
    //      what keeps `mem::copy`'s byte traffic and every FFI pointer outside the system entirely.
    //   2. **a reinterpretation needs `unsafe`.** `ptr<uint32>(&$a_float)` is the one operation that
    //      can put two differently-typed accesses over one address, and it is a word the author
    //      writes rather than something that happens by accident (AST::cast_reinterprets_pointee).
    //
    // Drop (1) and this is C's strict aliasing over a language with unrestricted pointer casts. Keep
    // it and what is left is the part that is actually true: two *typed places*, each reached without
    // ever leaving the compiler's own accounting, cannot overlap unless their types agree.
    //
    // **`byte` is the ancestor of every typed leaf, not their sibling.** that is what makes a `uint8`
    // access alias everything below it, which is the shape a byte-wise view of memory needs. The
    // other way round is the classic TBAA bug: an unrelated sibling would let a byte write be
    // reordered past a typed one.
    //
    //     root
    //      +-- byte            <-- int8, uint8, bool answer this node itself
    //           +-- integer16
    //           +-- integer32
    //           +-- integer64
    //           +-- float32
    //           +-- float64
    //           +-- address     --> every pointer, borrow, class handle, weak, callable
    //           +-- runtime.*   --> the compiler's own structures, which no Echo type can name
    //
    // **the shape of this graph is not decided here.** `AST::access_family_of` is the one table, and
    // it is a *language* rule before it is metadata.
    //
    // Here is why that ordering matters. An `unsafe` promotion asks the author to assert that
    // accessing storage as `T` is compatible with every other typed access that may alias it. That
    // obligation is only statable if the relation the optimizer acts on is the one the language
    // documents. So there is deliberately no second switch in this file to drift from the first: it
    // turns families into nodes and knows nothing about which type is in which family.
    class TbaaTree
    {
    public:
        explicit TbaaTree(llvm::LLVMContext &context);

        // the access tag for a scalar of this type, or null when the type has no honest leaf - a
        // struct by value, a void, an unresolved type parameter. null means *emit nothing*, which is
        // the conservative answer rather than a missing one
        llvm::MDNode *scalar_tag(const AST::ValueType &type);

        // the tag for the compiler's own structures - a refcount header, a vtable slot, the process
        // globals. they are storage no Echo type can name, so nothing an Echo program does can
        // legitimately alias one, and they get a leaf of their own rather than an integer's
        llvm::MDNode *runtime_tag(const std::string &name);

    private:
        llvm::MDBuilder _builder;

        llvm::MDNode *_root = nullptr;
        llvm::MDNode *_byte = nullptr;
        llvm::MDNode *_byte_tag = nullptr;

        // leaves are interned by name: one MDNode per type per unit, so two accesses at one type
        // carry the *same* node and LLVM's comparison is a pointer compare
        std::unordered_map<std::string, llvm::MDNode *> _leaves;

        // the access tag for the ancestor itself, which is what byte-family accesses carry
        llvm::MDNode *byte_tag();

        llvm::MDNode *leaf(const std::string &name);
    };
};

#endif
