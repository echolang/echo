#ifndef ABITABLECODEGEN_H
#define ABITABLECODEGEN_H

#pragma once

#include "AST/ASTBuiltin.h"

namespace AST
{
    class FunctionCallExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // ABI tables and the `abi::` builtins. its own subsystem for AtomicCodegen's
    // reason: one protocol, one file, and ExprCodegen already has too many arms.
    //
    // `emit_tables` plants a strong `__eco_abi_<mangled>` constant per exported
    // conformer in this unit - header plus adapter slots, DLLExport so
    // hide_non_exported_symbols keeps it. after bodies, so the statics the
    // slots adapt already exist. adapters come from TypeLowering::ensure_function_adapt
    class AbiCodegen
    {
    public:
        AbiCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void emit_tables();
        void gen_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

    private:
        CodegenContext &_ctx;

        const AST::ComplexType *subject(AST::FunctionCallExprNode &node, const char *name);

        void gen_fact(AST::FunctionCallExprNode &node);
        void gen_symbol(AST::FunctionCallExprNode &node);
        void gen_slot_symbol(AST::FunctionCallExprNode &node);
        void gen_bind_unchecked(AST::FunctionCallExprNode &node);
        void emit_table(AST::ComplexType &conformer, AST::ComplexType &iface);
    };
};

#endif
