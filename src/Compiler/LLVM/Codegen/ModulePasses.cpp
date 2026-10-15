#include "Compiler/LLVM/Codegen/ModulePasses.h"

#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>

void Compiler::LLVM::run_module_passes(
    llvm::Module &module,
    llvm::TargetMachine *target_machine,
    const std::function<void(llvm::PassBuilder &, llvm::ModulePassManager &)> &build
)
{
    // pass events for FunctionTimeTrace, only while the profiler is live
    llvm::PassInstrumentationCallbacks instrumentation;
    llvm::TimeProfilingPassesHandler time_profiling;

    if (llvm::timeTraceProfilerEnabled()) {
        time_profiling.registerCallbacks(instrumentation);
    }

    llvm::PassBuilder passBuilder(
        target_machine, llvm::PipelineTuningOptions(), std::nullopt, &instrumentation);
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;

    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    llvm::ModulePassManager modulePM;
    build(passBuilder, modulePM);

    modulePM.run(module, moduleAM);
}

void Compiler::LLVM::prepare_module(
    llvm::Module &module,
    llvm::TargetMachine *target_machine,
    bool optimize
)
{
    run_module_passes(module, target_machine, [optimize](llvm::PassBuilder &passBuilder, llvm::ModulePassManager &modulePM) {
        modulePM.addPass(llvm::GlobalDCEPass());

        if (optimize) {
            modulePM.addPass(passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2));
        }
    });
}
