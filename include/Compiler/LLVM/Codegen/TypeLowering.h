#ifndef TYPELOWERING_H
#define TYPELOWERING_H

#pragma once

#include "AST/ASTValueType.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/Codegen/ClassLayout.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace AST
{
    class Bundle;
    class FunctionDeclNode;
    class TypeDeclNode;
    class ComplexType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers echo types & declarations to their llvm equivalents: the primitive/struct type
    // mapping, the per-module function & struct declaration prepass, and lazy instantiation of a
    // generic struct instance on first use
    class TypeLowering
    {
    public:
        TypeLowering(CodegenContext &ctx) : _ctx(ctx) {};

        void create_cmp_units(const AST::Bundle &bundle);
        void build_function_maps(const AST::Bundle &bundle);
        void build_struct_maps(const AST::Bundle &bundle);

        llvm::Function *create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);
        llvm::StructType *create_llvm_struct_decl(const AST::TypeDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);

        // lowers a generic struct instantiation (an interned ComplexType with concrete property
        // types) to an llvm struct on first use, registering it in the compilation unit
        llvm::StructType *create_llvm_struct_for_instance(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit);

        // the heap block, payload and identity global of a class, lowered into this unit on first
        // use. separate from get_llvm_type because that answers what a class *value* is - an opaque
        // handle - and so cannot be the thing that lowers the layout: everything that needs the
        // layout (sizing an allocation, reaching the payload, touching the count, instanceof) asks
        // here instead. keyed on the ComplexType, so a generic instantiation and a class used from
        // another module both resolve
        ClassLayout get_or_create_class_layout(const AST::ComplexType *type, const Compiler::LLVM::CmpUnit &cmp_unit);

        llvm::Type *get_llvm_type(const AST::ValueType &type, const Compiler::LLVM::CmpUnit &cmp_unit);
        llvm::Type *get_llvm_type(const AST::ValueTypePrimitive type);

        // converts `value` from one echo type to another, emitting the widening, narrowing or
        // int/float conversion the pair calls for. returns the value unchanged when no
        // conversion is needed, and throws when the pair has no meaning
        //
        // keyed on ValueType rather than llvm::Type because signedness does not survive
        // lowering: i8 -> i32 is a sign extend for int8 and a zero extend for uint8, and the
        // llvm types are identical either way
        //
        // `from` may be void or unknown - BinaryExprNode::result_type() answers void whenever
        // its operands differ - in which case the value's own llvm type stands in for it and
        // `to` supplies the signedness
        llvm::Value *coerce_value(llvm::Value *value, const AST::ValueType &from, const AST::ValueType &to, const Compiler::LLVM::CmpUnit &cmp_unit);

    private:
        // wraps an already-lowered payload in its heap block and mints the class's typeinfo global
        // idempotent - a second call over a structure that already has a box does nothing
        void build_class_box(Structure &structure, const std::string &type_name, const Compiler::LLVM::CmpUnit &cmp_unit);

        CodegenContext &_ctx;
    };
};

#endif
