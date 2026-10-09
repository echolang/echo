#include "Compiler/LLVM/Codegen/LibraryVisibility.h"

#include "AST/FunctionDeclNode.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <unordered_set>

namespace Compiler::LLVM
{

void hide_non_exported_symbols(CmpUnit &unit)
{
    if (!unit.llvm_module) {
        return;
    }

    llvm::Module &module = *unit.llvm_module;

    std::unordered_set<llvm::Function *> exported;
    const FunctionTable &table = unit.function_table;

    for (function_id_t id = 1; id < table.size(); id++) {
        const Function &entry = table.get_function(id);

        if (entry.ast_funcdecl != nullptr
            && entry.ast_funcdecl->export_name.has_value()
            && entry.llvm_func != nullptr) {
            exported.insert(entry.llvm_func);
        }
    }

    for (llvm::Function &fn : module.functions()) {
        if (fn.isDeclaration()) {
            continue;
        }

        if (exported.count(&fn) != 0) {
            fn.setVisibility(llvm::GlobalValue::DefaultVisibility);
            continue;
        }

        fn.setVisibility(llvm::GlobalValue::HiddenVisibility);
        fn.setDSOLocal(true);
    }

    for (llvm::GlobalVariable &gv : module.globals()) {
        if (gv.isDeclaration()) {
            continue;
        }

        if (gv.getName().starts_with("llvm.")) {
            continue;
        }

        gv.setVisibility(llvm::GlobalValue::HiddenVisibility);
        gv.setDSOLocal(true);
    }
}

};
