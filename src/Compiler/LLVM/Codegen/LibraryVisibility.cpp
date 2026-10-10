#include "Compiler/LLVM/Codegen/LibraryVisibility.h"

#include "AST/FunctionDeclNode.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/SymbolTable.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <string>
#include <unordered_set>

namespace Compiler::LLVM
{

void hide_non_exported_symbols(CodegenContext &ctx)
{
    std::unordered_set<std::string> exported;

    for (auto &unit : ctx.cmp_units) {
        const FunctionTable &table = unit->function_table;
        for (function_id_t id = 1; id < table.size(); id++) {
            const Function &entry = table.get_function(id);
            if (entry.ast_funcdecl != nullptr && entry.ast_funcdecl->export_name.has_value()) {
                exported.insert(*entry.ast_funcdecl->export_name);
            }
        }
    }

    for (auto &unit : ctx.cmp_units) {
        if (!unit->llvm_module) {
            continue;
        }

        llvm::Module &module = *unit->llvm_module;

        for (llvm::Function &fn : module.functions()) {
            if (fn.isDeclaration()) {
                continue;
            }

            bool keep = exported.count(fn.getName().str()) > 0;
            if (!keep && fn.hasFnAttribute("wasm-export-name")) {
                keep = exported.count(
                    fn.getFnAttribute("wasm-export-name").getValueAsString().str()) > 0;
            }

            if (keep) {
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
}

};
