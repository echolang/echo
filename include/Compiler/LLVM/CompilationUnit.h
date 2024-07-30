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

namespace Compiler::LLVM
{
    struct CmpUnit
    {
        AST::Module *ast_module = nullptr;
        std::unique_ptr<llvm::Module> llvm_module = nullptr;

        FunctionTable function_table;
    };
}



#endif