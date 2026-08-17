#ifndef ATOMICCODEGEN_H
#define ATOMICCODEGEN_H

#pragma once

#include "AST/ASTBuiltin.h"

namespace AST
{
    class FunctionCallExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct LValue;

    // the seven `mem::atomic::` verbs. sequentially consistent, over a borrow of a word.
    //
    // its own subsystem for AbortCodegen's reason: one protocol, one file, and ExprCodegen
    // already has too many arms. the class-count orderings live in CountAtomics.h and are
    // a different protocol - this surface does not take an ordering because nothing in the
    // language can check one
    class AtomicCodegen
    {
    public:
        AtomicCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_atomic_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

    private:
        CodegenContext &_ctx;

        // argument 0 is the address of a place. the same invariant take/init carry, asked
        // here so this file does not reach into ExprCodegen
        LValue slot_of(AST::FunctionCallExprNode &node, const char *name, size_t arity);
    };
};

#endif
