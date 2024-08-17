#include "Compiler/LLVM/SymbolTable.h"

#include "AST/StructNode.h"

Compiler::LLVM::structure_id_t Compiler::LLVM::StructureTable::push_structure(const AST::StructDeclNode *structdecl, llvm::StructType *structtype)
{
    _structures.push_back({ structdecl, structtype });
    structure_id_t handle = _structures.size() - 1;

    _struct_ast_map[structdecl] = handle;
    
    auto struct_value_type = structdecl->value_type();
    if (struct_value_type.is_struct() || struct_value_type.is_class()) {
        _struct_type_map[struct_value_type.get_complex_type()] = handle;
    }

    return handle;
}

Compiler::LLVM::structure_id_t Compiler::LLVM::StructureTable::push_structure(const AST::ComplexType *type, llvm::StructType *structtype)
{
    _structures.push_back({ nullptr, structtype });
    structure_id_t handle = _structures.size() - 1;

    _struct_type_map[type] = handle;

    return handle;
}