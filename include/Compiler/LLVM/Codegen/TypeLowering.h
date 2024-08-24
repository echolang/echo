#ifndef TYPELOWERING_H
#define TYPELOWERING_H

#pragma once

#include "AST/ASTValueType.h"
#include "Compiler/LLVM/CompilationUnit.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace AST
{
    class Bundle;
    class FunctionDeclNode;
    class StructDeclNode;
    class ComplexType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers echo types & declarations to their llvm equivalents: the primitive/struct type
    // mapping, the per-module function & struct declaration prepass, and lazy instantiation of a
    // generic struct instance on first use.
    class TypeLowering
    {
    public:
        TypeLowering(CodegenContext &ctx) : _ctx(ctx) {};

        void create_cmp_units(const AST::Bundle &bundle);
        void build_function_maps(const AST::Bundle &bundle);
        void build_struct_maps(const AST::Bundle &bundle);

        llvm::Function *create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);
        llvm::StructType *create_llvm_struct_decl(const AST::StructDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);

        // lowers a generic struct instantiation (an interned ComplexType with concrete property
        // types) to an llvm struct on first use, registering it in the compilation unit.
        llvm::StructType *create_llvm_struct_for_instance(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit);

        llvm::Type *get_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit);
        llvm::Type *get_llvm_type(const AST::ValueTypePrimitive type);

    private:
        CodegenContext &_ctx;
    };
};

#endif
