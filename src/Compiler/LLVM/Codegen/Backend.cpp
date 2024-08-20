#include "Compiler/LLVM/Codegen/Backend.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Analysis/InlineCost.h>

#include <cstdlib>

namespace Compiler::LLVM
{
void Backend::print_ir(bool to_file)
{ 
    auto main = _ctx.main_cmp_unit();
    main->llvm_module->print(llvm::outs(), nullptr);
}

void Backend::run_code() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto main_cmp_unit = _ctx.main_cmp_unit();
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException("No main module found to run", nullptr);
    }

    std::string errorStr;
    const llvm::TargetOptions opts;
    llvm::ExecutionEngine *EE = llvm::EngineBuilder(std::move(_ctx.main_cmp_unit()->llvm_module))
        .setErrorStr(&errorStr)
        .setEngineKind(llvm::EngineKind::JIT)
        .setTargetOptions(opts)
        .create();

    _ctx.main_cmp_unit()->llvm_module = nullptr;

    if (!EE) {
        llvm::errs() << "Failed to create ExecutionEngine: " << errorStr << '\n';
        return;
    }

    // enable debugging

    EE->finalizeObject();

    auto *func = EE->FindFunctionNamed("main");
    if (!func) {
        llvm::errs() << "Function 'main' not found in module.\n";
        return;
    }

    std::vector<llvm::GenericValue> noargs;
    llvm::GenericValue gv = EE->runFunction(func, noargs);

    delete EE;
    // llvm::llvm_shutdown();
}

void Backend::make_exec(std::string executable_name)
{
    // llvm::InitializeAllTargetInfos();
    // llvm::InitializeAllTargets();
    // llvm::InitializeAllTargetMCs();
    // llvm::InitializeAllAsmParsers();
    // llvm::InitializeAllAsmPrinters();
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();



    auto TargetTriple = llvm::sys::getDefaultTargetTriple();
    // auto TargetTriple = "aarch64-linux-gnu";

    std::string Error;
    auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);
    if (!Target) {
        llvm::errs() << Error;
        return;
    }

    auto CPU = "generic";
    auto Features = "";

    llvm::TargetOptions opt;
    auto TargetMachine = Target->createTargetMachine(TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_);

    _ctx.current_module()->setDataLayout(TargetMachine->createDataLayout());
    _ctx.current_module()->setTargetTriple(TargetTriple);

    std::error_code EC;
    std::string objectFileName = executable_name + ".o";
    llvm::raw_fd_ostream dest(objectFileName, EC, llvm::sys::fs::OF_None);

    if (EC) {
        llvm::errs() << "Could not open file: " << EC.message();
        return;
    }

    llvm::legacy::PassManager pass;
    auto FileType = llvm::CodeGenFileType::ObjectFile;

    if (TargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
        llvm::errs() << "TargetMachine can't emit a file of this type";
        return;
    }

    pass.run(*_ctx.current_module());
    dest.flush();

    llvm::outs() << "Generated object file: " << objectFileName << "\n";

    std::string command = "clang -o " + executable_name + " " + objectFileName;
    int result = std::system(command.c_str());
    if (result != 0) {
        llvm::errs() << "Error: linking failed\n";
        return;
    }

    llvm::outs() << "Executable \"" << executable_name << "\" created successfully\n";
}

void Backend::optimize() {
    if (!_ctx.current_module()) {
        llvm::errs() << "Module is not initialized.\n";
        return;
    }

    llvm::PassBuilder passBuilder;
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;
    
    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    // make the pipeline
    llvm::ModulePassManager modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    modulePM.addPass(llvm::ModuleInlinerPass(llvm::getInlineParams(3, 0), llvm::InliningAdvisorMode::Default,
                                  llvm::ThinOrFullLTOPhase::None));
    
    modulePM.run(*_ctx.current_module(), moduleAM);
}
}
