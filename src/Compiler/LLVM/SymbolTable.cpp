#include "Compiler/LLVM/SymbolTable.h"

#include "AST/StructNode.h"

Compiler::LLVM::structure_id_t Compiler::LLVM::StructureTable::push_structure(const AST::StructDeclNode *structdecl, llvm::StructType *structtype)
{
    _structures.push_back({ structdecl, structtype });
    structure_id_t handle = _structures.size() - 1;

    _struct_ast_map[structdecl] = handle;
    _struct_type_map[structdecl->value_type().get_complex_type()] = handle;
    return handle;
}