#include "Compiler/LLVM/SymbolTable.h"

#include "AST/TypeDeclNode.h"

Compiler::LLVM::structure_id_t Compiler::LLVM::StructureTable::push_structure(const AST::TypeDeclNode *structdecl, llvm::StructType *structtype)
{
    Structure entry{};
    entry.ast_structdecl = structdecl;
    entry.llvm_struct = structtype;

    _structures.push_back(entry);
    structure_id_t handle = _structures.size() - 1;

    _struct_ast_map[structdecl] = handle;

    // only a type with a layout gets a structure entry - an interface is never lowered to an llvm
    // struct, so mapping its ComplexType onto a structure handle would hand codegen a payload type
    // built from zero properties and let a lookup succeed where it should not
    auto struct_value_type = structdecl->value_type();
    if (struct_value_type.has_property_layout()) {
        _struct_type_map[struct_value_type.get_complex_type()] = handle;
    }

    return handle;
}

Compiler::LLVM::structure_id_t Compiler::LLVM::StructureTable::push_structure(const AST::ComplexType *type, llvm::StructType *structtype)
{
    Structure entry{};
    entry.llvm_struct = structtype;

    _structures.push_back(entry);
    structure_id_t handle = _structures.size() - 1;

    _struct_type_map[type] = handle;

    return handle;
}
