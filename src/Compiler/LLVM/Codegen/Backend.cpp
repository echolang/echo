#include "Compiler/LLVM/Codegen/Backend.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
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

#include <fmt/core.h>

#include <cstdio>
#include <cstdlib>

#include <filesystem>
#include <optional>
#include <vector>

namespace Compiler::LLVM
{
Backend::Backend(CodegenContext &ctx) : _ctx(ctx)
{
}

Backend::~Backend() = default;

void Backend::print_ir(bool to_file)
{
    auto main = _ctx.main_cmp_unit();
    main->llvm_module->print(llvm::outs(), nullptr);
}

void Backend::run_code()
{
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

void Backend::init_target()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    _ctx.target_triple = llvm::sys::getDefaultTargetTriple();

    std::string error;
    auto *target = llvm::TargetRegistry::lookupTarget(_ctx.target_triple, error);
    if (!target) {
        throw Compiler::InternalCompilerException(fmt::format(
            "Could not resolve the host target '{}': {}", _ctx.target_triple, error));
    }

    llvm::TargetOptions opt;
    _target_machine.reset(
        target->createTargetMachine(_ctx.target_triple, "generic", "", opt, llvm::Reloc::PIC_));
    if (!_target_machine) {
        throw Compiler::InternalCompilerException(fmt::format(
            "Could not create a target machine for '{}'", _ctx.target_triple));
    }

    _ctx.data_layout = _target_machine->createDataLayout();
}

namespace
{

// the linker to invoke, as an argv. `std::nullopt` means "we do not know how to spell this platform" and
// the caller falls back to the clang driver.
//
// **the fallback is the point.** Going straight to `ld` saves ~33ms of clang driver startup on every
// single build, which matters because linking is otherwise a constant this compiler cannot optimize away -
// but a platform whose flags are not written here must still build, and clang is what knows them
#if defined(__APPLE__)
// the SDK holds libSystem, and its path is not fixed - it moves with every Xcode update, so it has to be
// asked for rather than assumed.
//
// **asked once.** `xcrun` costs ~6 ms warm, which is a fifth of what going straight to `ld` saves in the
// first place, and the answer cannot change during a compile. $SDKROOT first, because that is the variable
// xcrun itself honours and a caller who set it is telling us not to guess
const std::string &host_sdk_root()
{
    static const std::string answer = [] {
        if (const char *from_env = std::getenv("SDKROOT"); from_env != nullptr && *from_env != '\0') {
            return std::string(from_env);
        }

        std::string out;

        if (FILE *pipe = popen("xcrun --show-sdk-path 2>/dev/null", "r")) {
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                out += buffer;
            }

            if (pclose(pipe) != 0) {
                return std::string();
            }
        }

        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
            out.pop_back();
        }

        return out;
    }();

    return answer;
}
#endif

void append_objects(
    std::vector<std::string> &argv, const std::vector<std::filesystem::path> &objects)
{
    for (const std::filesystem::path &object : objects) {
        argv.push_back(object.string());
    }
}

std::optional<std::vector<std::string>> host_linker_command(
    const std::string &executable_name, const std::vector<std::filesystem::path> &objects)
{
#if defined(__APPLE__)
    const std::string &sdk_root = host_sdk_root();

    if (sdk_root.empty()) {
        return std::nullopt;
    }

#if defined(__aarch64__) || defined(__arm64__)
    const std::string arch = "arm64";
#elif defined(__x86_64__)
    const std::string arch = "x86_64";
#else
    return std::nullopt;
#endif

    std::vector<std::string> command = { "ld", "-o", executable_name };
    append_objects(command, objects);
    command.push_back("-lSystem");
    command.push_back("-syslibroot");
    command.push_back(sdk_root);
    command.push_back("-arch");
    command.push_back(arch);

    return command;
#else
    (void)executable_name;
    (void)objects;
    return std::nullopt;
#endif
}

// runs one argv and answers whether it succeeded. **as an argv, with no shell in between**: an SDK path or
// an output name may contain a space, and llvm::sys::ExecuteAndWait takes the words as words - the quoting
// this replaces wrapped each in `"` and had no answer at all for a path containing one, besides spawning a
// /bin/sh on top of the linker to interpret it
bool run_command(const std::vector<std::string> &argv)
{
    llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(argv.front());

    if (!program) {
        return false;
    }

    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    args.front() = program.get();

    return llvm::sys::ExecuteAndWait(program.get(), args) == 0;
}

};

bool Backend::emit_object(Compiler::LLVM::CmpUnit &cmp_unit, const std::filesystem::path &object_path)
{
    // the same TargetMachine codegen took its data layout from (Backend::init_target), so the
    // object can never be emitted for a target the IR was not laid out for
    if (!_target_machine) {
        llvm::errs() << "No target resolved - init_target must run before emit_object\n";
        return false;
    }

    if (!cmp_unit.llvm_module) {
        llvm::errs() << "Module for unit '" << cmp_unit.ast_module->name
                     << "' has already been consumed - nothing to emit\n";
        return false;
    }

    std::error_code ec;

    if (object_path.has_parent_path()) {
        std::filesystem::create_directories(object_path.parent_path(), ec);
    }

    llvm::raw_fd_ostream dest(object_path.string(), ec, llvm::sys::fs::OF_None);

    if (ec) {
        llvm::errs() << "Could not open file: " << ec.message();
        return false;
    }

    llvm::legacy::PassManager pass;
    auto FileType = llvm::CodeGenFileType::ObjectFile;

    if (_target_machine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
        llvm::errs() << "TargetMachine can't emit a file of this type";
        return false;
    }

    pass.run(*cmp_unit.llvm_module);
    dest.flush();

    return true;
}

bool Backend::link_executable(
    const std::string &executable_name, const std::vector<std::filesystem::path> &objects)
{
    if (objects.empty()) {
        llvm::errs() << "Error: nothing to link\n";
        return false;
    }

    if (const auto command = host_linker_command(executable_name, objects)) {
        if (run_command(command.value())) {
            return true;
        }

        // not a hard failure: the flags above are a guess about the host, and clang below knows better.
        // A linker error in the *program* will be reported by clang in a moment anyway
        llvm::errs() << "Note: the system linker failed, retrying through the clang driver\n";
    }

    std::vector<std::string> fallback = { "clang", "-o", executable_name };
    append_objects(fallback, objects);

    if (!run_command(fallback)) {
        llvm::errs() << "Error: linking failed\n";
        return false;
    }

    return true;
}

bool Backend::make_exec(std::string executable_name)
{
    Compiler::LLVM::CmpUnit *main_cmp_unit = _ctx.main_cmp_unit();
    if (!main_cmp_unit) {
        llvm::errs() << "No main module found to emit\n";
        return false;
    }

    const std::filesystem::path object_path = executable_name + ".o";

    if (!emit_object(*main_cmp_unit, object_path)) {
        return false;
    }

    return link_executable(executable_name, { object_path });
}

void Backend::optimize()
{
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
};
