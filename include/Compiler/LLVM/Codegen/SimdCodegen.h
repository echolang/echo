#ifndef SIMDCODEGEN_H
#define SIMDCODEGEN_H

#pragma once

#include "AST/ASTBuiltin.h"
#include "AST/ASTValueType.h"

#include <llvm/Support/Alignment.h>

namespace llvm
{
    class FixedVectorType;
    class Value;
};

namespace AST
{
    class BinaryExprNode;
    class FunctionCallExprNode;
    class UnaryExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the `simd::` verbs, the elementwise operators, and packed-vector alignment.
    //
    // its own subsystem for AtomicCodegen's reason: one protocol, one file, and ExprCodegen
    // already has too many arms. bitmask of `<N x i1>`: N<=8 is a portable bitcast to iN
    // (zext to i64); N=16 is sext-to-i8 plus two 8-lane `llvm.vector.reduce.add`s, because
    // a zext AND drops the 0x80 power (`1 & 0x80` is 0)
    class SimdCodegen
    {
    public:
        SimdCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_simd_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

        // elementwise `+ - * / & | ^ << >>` and the comparisons. ExprCodegen keeps the
        // arm *position* (after pointer, before integer) and forwards
        void gen_binary(AST::BinaryExprNode &node, const AST::ValueType &lhs, const AST::ValueType &rhs, llvm::Value *left, llvm::Value *right);

        void gen_unary(AST::UnaryExprNode &node, const AST::ValueType &type, llvm::Value *value);

        // broadcast `scalar` across every lane of `vec_ty`. the scalar's LLVM type must
        // already be the element type
        llvm::Value *splat(llvm::Value *scalar, llvm::FixedVectorType *vec_ty);

        // a packed vector access uses the element's alignment, not the vector's. that is
        // what lets a `ptr<uint8>` group load never become a faulting `movdqa`
        llvm::Align packed_alignment(const AST::ValueType &vec);

    private:
        CodegenContext &_ctx;

        llvm::Value *eval_arg(AST::FunctionCallExprNode &node, size_t index);
        unsigned bound_n(AST::FunctionCallExprNode &node, size_t arg_index);
    };
};

#endif
