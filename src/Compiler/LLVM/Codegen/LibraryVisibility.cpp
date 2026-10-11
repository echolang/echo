#include "Compiler/LLVM/Codegen/LibraryVisibility.h"

#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <unordered_set>

namespace Compiler::LLVM
{

namespace
{
    // functions llvm.global_ctors / llvm.global_dtors will call. hiding one
    // makes Mach-O drop the `__mod_term_func` entry, so a lib fini never
    // runs. Windows registers that fini with `atexit` from a ctor, so the
    // ctor is in this set too
    std::unordered_set<const llvm::Function *> lifecycle_functions(llvm::Module &module)
    {
        std::unordered_set<const llvm::Function *> keep;

        for (const char *name : { "llvm.global_ctors", "llvm.global_dtors" }) {
            llvm::GlobalVariable *list = module.getGlobalVariable(name);

            if (list == nullptr || !list->hasInitializer()) {
                continue;
            }

            auto *array = llvm::dyn_cast<llvm::ConstantArray>(list->getInitializer());

            if (array == nullptr) {
                continue;
            }

            for (llvm::Use &use : array->operands()) {
                auto *entry = llvm::dyn_cast<llvm::ConstantStruct>(use.get());

                if (entry == nullptr || entry->getNumOperands() < 2) {
                    continue;
                }

                if (auto *fn = llvm::dyn_cast<llvm::Function>(entry->getOperand(1))) {
                    keep.insert(fn);
                }
            }
        }

        return keep;
    }
}

void hide_non_exported_symbols(CodegenContext &ctx)
{
    for (auto &unit : ctx.cmp_units) {
        if (!unit->llvm_module) {
            continue;
        }

        llvm::Module &module = *unit->llvm_module;
        const std::unordered_set<const llvm::Function *> lifecycle = lifecycle_functions(module);

        for (llvm::Function &fn : module.functions()) {
            if (fn.isDeclaration()) {
                continue;
            }

            // DLLExport is planted at lowering on every native-lib `#[export]`.
            // the marker is the keep-visible rule, so a table global Phase 3
            // plants with the same storage class is kept without a name set
            if (fn.hasDLLExportStorageClass() || lifecycle.count(&fn) > 0) {
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

            if (gv.hasDLLExportStorageClass()) {
                gv.setVisibility(llvm::GlobalValue::DefaultVisibility);
                continue;
            }

            gv.setVisibility(llvm::GlobalValue::HiddenVisibility);
            gv.setDSOLocal(true);
        }
    }
}

};
