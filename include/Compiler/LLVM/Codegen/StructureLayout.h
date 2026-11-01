#ifndef STRUCTURELAYOUT_H
#define STRUCTURELAYOUT_H

#pragma once

#include "Compiler/LLVM/SymbolTable.h"

#include <functional>

namespace AST
{
    class ComplexType;
    class ValueType;
};

namespace llvm
{
    class DataLayout;
    class LLVMContext;
    class StructType;
    class Type;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // sequential properties, or a packed enum overlay. false when a field could not be
    // lowered. a non-opaque struct is already filled. TypeLayout's size query and
    // fill_structure_body both ask, so a stride and a GEP agree
    bool fill_aggregate_llvm_type(
        llvm::StructType *st,
        const AST::ComplexType &type,
        llvm::LLVMContext &context,
        const llvm::DataLayout &layout,
        const std::function<llvm::Type *(const AST::ValueType &)> &lower
    );

    // **the machine shape of a ValueType**, one switch. TypeLayout's size query and
    // TypeLowering::get_llvm_type both ask, so a `const if (mem::size<T>())` and a GEP
    // cannot disagree. `aggregate` interns a struct/enum (and a tagged optional): TypeLayout
    // fills a placeholder, TypeLowering registers it in the unit table. null when the type
    // has no layout yet - opaque, unbound length, a type parameter
    llvm::Type *llvm_type_of(
        const AST::ValueType &type,
        llvm::LLVMContext &context,
        const llvm::DataLayout &layout,
        const std::function<llvm::Type *(const AST::ComplexType *)> &aggregate
    );

    // the LLVM body of a registered aggregate: one field per property, or a packed enum overlay.
    // takes the id, not a Structure&, because lowering a field type can push another structure
    // and grow the table. build_class_box is the same split
    //
    // `lower` is TypeLowering::get_llvm_type. every aggregate then has property_byte_offset
    // filled from the DataLayout, so a property's address and its DWARF offset share one table
    void fill_structure_body(
        structure_id_t struct_id,
        const AST::ComplexType &type,
        StructureTable &table,
        llvm::LLVMContext &context,
        const llvm::DataLayout &layout,
        CodegenContext &ctx,
        const std::function<llvm::Type *(const AST::ValueType &)> &lower
    );
};

#endif
