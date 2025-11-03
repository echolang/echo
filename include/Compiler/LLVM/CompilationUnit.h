#ifndef COMPILATIONUNIT_H
#define COMPILATIONUNIT_H

#pragma once

#include "AST/ASTModule.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ASTMangler.h"

#include "Compiler/LLVM/SymbolTable.h"

#include <llvm/IR/Module.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Compiler::LLVM
{
    struct CmpUnit
    {
        AST::Module *ast_module = nullptr;
        std::unique_ptr<llvm::Module> llvm_module = nullptr;

        FunctionTable function_table;

        std::unique_ptr<StructureTable> structure_table = nullptr;

        // the ODR-shared definitions this unit still owes a body for.
        //
        // a generated definition - a generic instantiation, a synthesized deinit or copy constructor, a
        // field-wise constructor - is not owned by any one module, so it is defined in every unit that
        // *references* it rather than once in the unit that happens to hold its declaration node. This is
        // the queue of the ones discovered so far, filled by TypeLowering::create_llvm_func_decl and
        // drained by LLVMCompiler::drain_pending_definitions.
        //
        // it has to be a queue rather than a set computed up front, because the set is not knowable up
        // front: emitting one of these bodies is what discovers the next. `array<Padded>::reserve` calls
        // `mem::realloc<Padded>`, and an interface vtable names an implementation lazily, at the widening
        // site, after every up-front scan has finished.
        //
        // **a vector, never a set** - the order definitions are appended in is the order they appear in
        // the emitted module, so a hash container would make every IR dump depend on pointer hashing
        std::vector<const AST::FunctionDeclNode *> pending_definitions;
        std::unordered_set<const AST::FunctionDeclNode *> definition_queued;

        CmpUnit() {
            function_table = FunctionTable();
            structure_table = std::make_unique<StructureTable>();
        }
    };
};



#endif