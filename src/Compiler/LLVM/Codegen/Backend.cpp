#include "Compiler/LLVM/Codegen/Backend.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/TargetFacts.h"

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
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Internalize.h>
#include <llvm/Analysis/InlineCost.h>

#include <fmt/core.h>

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
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

// **every unit, in order, as the object writer will see it.** `print_ir` prints one merged module, which is
// what `-O` produces and therefore what almost every IR golden pins - and that left the *ordinary* build path
// with no way to be looked at or asserted on at all, which is exactly where Backend::optimize_unit now runs.
//
// each unit is optimized first, deliberately: a dump of what the emitter is about to be handed is worth
// having, and a dump of something else is a check that pins nothing. `--no-optimize` is how to see the raw IR
//
// read off the options rather than taken as a parameter, because the promise above is only true while this
// and LLVMCompiler::emit_objects ask the same thing of the same source. optimize_unit is idempotent, so the
// emit that follows this dump re-optimizes nothing
void Backend::print_unit_ir()
{
    for (auto &cmp_unit : _ctx.cmp_units) {
        if (!cmp_unit->llvm_module) {
            continue;
        }

        if (!_ctx.options.no_optimize) {
            optimize_unit(*cmp_unit);
        }

        // a header per unit, in the `[section]` shape --print-symbol-table and the measurement dumps use,
        // so a golden can anchor on the unit it means rather than on whatever came first
        llvm::outs() << "[unit " << cmp_unit->ast_module->name << "]\n";
        cmp_unit->llvm_module->print(llvm::outs(), nullptr);
    }
}

int Backend::run_code(const std::vector<std::string> &arguments, const char *const *environment)
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto main_cmp_unit = _ctx.main_cmp_unit();
    if (!main_cmp_unit) {
        throw Compiler::InternalCompilerException("No main module found to run", nullptr);
    }

    // before the EngineBuilder below, which moves the module out - and inside run_code rather than beside
    // it, because the JIT is the only place the prune is sound. `-t` sees it either way: PhaseTimings is
    // process-wide precisely so a phase can be claimed by whichever object owns the work
    {
        Compiler::ScopedPhase phase("prune");
        prune_to_entry();
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
        return 1;
    }

    // enable debugging

    EE->finalizeObject();

    auto *func = EE->FindFunctionNamed(ECO_ENTRY_SYMBOL_NAME);
    if (!func) {
        llvm::errs() << "Function '" ECO_ENTRY_SYMBOL_NAME "' not found in module.\n";
        return 1;
    }

    // runFunctionAsMain rather than runFunction with no arguments: it is the one place that knows how to
    // marshal argc/argv/envp into a call, and the entry point now takes all three. It dispatches on the
    // parameter count and report_fatal_error()s on a shape it does not recognise, so the agreement with
    // LLVMCompiler's FunctionType is load-bearing and pinned by tests_eco/env
    int status = EE->runFunctionAsMain(func, arguments, environment);

    delete EE;
    // llvm::llvm_shutdown();

    return status;
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

    // Compiler::TargetFacts owns "what architecture is this", and derives it from the same default
    // triple Backend::init_target hands the TargetMachine - so what is compiled and what is linked
    // cannot disagree. Spelled here as its own `#if defined(__aarch64__)` chain, they could
    const std::string arch = Compiler::TargetFacts::host().architecture;

    if (arch.empty()) {
        return std::nullopt;
    }

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

// guards the module, builds the four analysis managers and cross-registers the proxies, then runs
// whatever pipeline `build` filled in. Every module-pass entry point below goes through here, so how
// analyses are registered is decided once - two spellings of it drift silently, and a pass that then
// asks for an analysis nobody registered aborts inside LLVM rather than here.
//
// file-local rather than a member: llvm::ModulePassManager is a template alias, so a declaration in
// Backend.h would drag the whole pass infrastructure into every translation unit that includes it
static void run_module_passes(
    llvm::Module &module,
    const std::function<void(llvm::PassBuilder &, llvm::ModulePassManager &)> &build)
{
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

    llvm::ModulePassManager modulePM;
    build(passBuilder, modulePM);

    modulePM.run(module, moduleAM);
}

void Backend::optimize()
{
    if (!_ctx.current_module()) {
        llvm::errs() << "Module is not initialized.\n";
        return;
    }

    run_module_passes(*_ctx.current_module(), [](llvm::PassBuilder &passBuilder, llvm::ModulePassManager &modulePM) {
        // **the O3 pipeline and nothing after it.** this used to append a second ModuleInlinerPass once
        // the pipeline had already finished, which is a shape worth naming so it is not added back:
        // whatever that pass inlined was never simplified again. the O3 pipeline interleaves its inliner
        // with SROA, instcombine and GVN precisely so that an inlined body gets cleaned up, and a round
        // bolted on the end gets none of that - it could only ever grow the module.
        //
        // the goldens that care are tests_eco/iteration/lowered_cost (no calls at all in a foreach body),
        // modules/cross_module_inline and native/class_on_the_heap (the whole refcount runtime removed).
        // all three still hold on the pipeline's own inliner, which is the evidence the extra round was
        // not buying them
        modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    });
}

void Backend::optimize_unit(Compiler::LLVM::CmpUnit &cmp_unit)
{
    if (!cmp_unit.llvm_module || cmp_unit.optimized) {
        return;
    }

    cmp_unit.optimized = true;

    // **O2 per unit, and the reason it is not O3 is not timidity - it is that this one has to stay
    // cheap.** `Backend::optimize` runs once, over a merged module, for a build that asked for it. This
    // runs on every unit of every ordinary `echoc build`, whose whole point is that it is the fast path.
    //
    // it exists because that path previously ran **no IR pass at all**: no mem2reg, no SROA, no inlining.
    // Every parameter of every function is spilled to an entry alloca by StmtCodegen and reloaded per use,
    // every accessor is a real call, and CodegenContext::entry_alloca goes to the trouble of putting slots
    // where mem2reg can find them - for a pipeline that was never run. The backend was already at -O2 for
    // instruction selection, so the IR was the only part left unoptimized.
    //
    // **per unit, which is what keeps it compatible with the object cache.** a pipeline over one unit is a
    // pure function of that unit's IR, so a cached object stays a function of its own sources plus its
    // dependencies' keys - which is what Compiler::compute_module_keys promises and tests/module_cache.cpp
    // pins byte for byte. Whole-program `-O` is still merge-then-O3 and still bypasses the cache; the two
    // are no longer all or nothing, which is the actual change here
    run_module_passes(*cmp_unit.llvm_module, [](llvm::PassBuilder &passBuilder, llvm::ModulePassManager &modulePM) {
        modulePM = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    });
}

// the module's own function definitions, by symbol name.
//
// declarations are deliberately not among them: InternalizePass leaves them alone, so they were never
// candidates for the prune to drop - they are how the JIT resolves printf, malloc, free and every
// `extern` symbol out of the host process
static std::vector<std::string> definition_names(const llvm::Module *module)
{
    std::vector<std::string> names;

    if (!module) {
        return names;
    }

    for (const llvm::Function &function : module->functions()) {
        if (!function.isDeclaration()) {
            names.push_back(function.getName().str());
        }
    }

    return names;
}

// how many of them there are, which the "before" half of the report is all that wants - a name vector
// built to be measured and dropped is one heap copy per definition for a number
static size_t definition_count(const llvm::Module *module)
{
    if (!module) {
        return 0;
    }

    return static_cast<size_t>(std::count_if(
        module->begin(), module->end(),
        [](const llvm::Function &function) { return !function.isDeclaration(); }));
}

void Backend::prune_to_entry()
{
    // taken before the passes run, so the report can say what was *removed* rather than only what is left.
    // A prune that dropped nothing at all - a root predicate that stopped matching, a module the pass
    // manager declined - reads as two equal numbers, which is the failure this diagnostic exists to catch
    const size_t before = definition_count(_ctx.current_module());

    if (!_ctx.current_module()) {
        llvm::errs() << "Module is not initialized.\n";
        return;
    }

    run_module_passes(*_ctx.current_module(), [](llvm::PassBuilder &, llvm::ModulePassManager &modulePM) {
        // internalize first: GlobalDCE can only delete what nothing outside the module could call, and
        // codegen hands it a module in which almost everything is externally linked.
        //
        // declarations are left alone by InternalizePass, which is what keeps the JIT able to resolve
        // printf, malloc, free and every `extern` symbol out of the host process
        modulePM.addPass(llvm::InternalizePass([](const llvm::GlobalValue &gv) {
            return gv.getName() == ECO_ENTRY_SYMBOL_NAME;
        }));

        modulePM.addPass(llvm::GlobalDCEPass());
    });

    // sorted rather than left in module order: which body LLVM happens to hold first is a property of how
    // codegen emitted them, and a report keyed on that moves whenever an unrelated emission order does
    std::vector<std::string> survivors = definition_names(_ctx.current_module());
    std::sort(survivors.begin(), survivors.end());

    // the symbol names, so a survivor lines up with what a `-p` dump calls it
    _prune_report = fmt::format("[prune]\n  {} function definitions, {} reachable from {}\n",
        before, survivors.size(), ECO_ENTRY_SYMBOL_NAME);

    for (const std::string &name : survivors) {
        _prune_report += fmt::format("    {}\n", name);
    }
}
};
