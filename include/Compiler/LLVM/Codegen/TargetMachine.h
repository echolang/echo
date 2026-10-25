#ifndef TARGETMACHINE_H
#define TARGETMACHINE_H

#pragma once

#include "Compiler/CompilerOptions.h"

#include <llvm/Support/CodeGen.h>
#include <llvm/Target/TargetOptions.h>

#include <memory>
#include <string>

namespace llvm
{
    class TargetMachine;
};

namespace Compiler::LLVM
{
    // make_target_machine and the JIT EngineBuilder both read this, or `run`
    // silently ISels a different machine than emit. LoopAlignment stays 0:
    // LLVM's PrefLoopAlignment is the floor. TrapUnreachable is on; the
    // noreturn-after trap is off on wasm and Windows
    llvm::TargetOptions target_options_for(const std::string &triple);

    // None / Default / Aggressive from CodegenOpt. t_none is `-g` /
    // `--optimize none`: must not ISel at Default
    llvm::CodeGenOptLevel codegen_opt_level(Compiler::CodegenOpt opt);

    std::unique_ptr<llvm::TargetMachine> make_target_machine(
        const std::string &triple,
        const std::string &cpu,
        const std::string &features,
        llvm::CodeGenOptLevel opt_level,
        bool pic,
        std::string &error
    );
};

#endif
