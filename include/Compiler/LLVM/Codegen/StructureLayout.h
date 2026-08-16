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
    class Type;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the LLVM body of a registered aggregate: one field per property, or a packed enum overlay.
    // takes the id, not a Structure&, because lowering a field type can push another structure
    // and reallocate the table
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
