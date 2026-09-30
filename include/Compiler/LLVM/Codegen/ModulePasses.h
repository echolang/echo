#ifndef MODULEPASSES_H
#define MODULEPASSES_H

#pragma once

#include <llvm/Passes/PassBuilder.h>

#include <functional>

namespace llvm
{
    class Module;
    class TargetMachine;
};

namespace Compiler::LLVM
{
    // one PassBuilder and one set of analysis managers. O2, O3, and the JIT prune all
    // build different pass lists; this is the chassis they share
    void run_module_passes(
        llvm::Module &module,
        llvm::TargetMachine *target_machine,
        const std::function<void(llvm::PassBuilder &, llvm::ModulePassManager &)> &build
    );

    void prepare_module(llvm::Module &module, llvm::TargetMachine *target_machine, bool optimize);
};

#endif
