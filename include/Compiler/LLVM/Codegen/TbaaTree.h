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
    // TBAA tree, one per LLVMContext. a raw-pointer access is never tagged; `byte` is the ancestor
    // of every typed leaf (a sibling would let a byte write reorder past a typed one). families
    // come from AST::access_family_of: this file turns them into nodes and nothing else
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
