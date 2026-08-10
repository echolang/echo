#include "Compiler/LLVM/Codegen/Backend.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/HostTool.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/TargetFacts.h"
#include "Compiler/TargetSubtarget.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/DynamicLibrary.h>
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
    Compiler::ensure_native_target_registered();

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

    // **the JIT builds its own target machine, so it has to be told the same thing.** MCJIT does not
    // take `_target_machine` - it selects one out of the EngineBuilder - and left alone it defaults to
    // an empty CPU string. So `run` optimized the IR for one subtarget and then selected instructions
    // for another, and `--target-cpu` would have been a flag that quietly did nothing on this path.
    // The answer is the same call the pipelines above make, which is the whole point of it being a
    // function rather than state on the machine
    const Compiler::Subtarget sub = subtarget();
    const std::vector<std::string> attributes = split_target_features(sub.features);

    std::string errorStr;
    const llvm::TargetOptions opts;
    llvm::ExecutionEngine *EE = llvm::EngineBuilder(std::move(_ctx.main_cmp_unit()->llvm_module))
        .setErrorStr(&errorStr)
        .setEngineKind(llvm::EngineKind::JIT)
        .setTargetOptions(opts)
        .setMCPU(sub.cpu)
        .setMAttrs(attributes)
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

Compiler::Subtarget Backend::subtarget() const
{
    // **asked, never stored.** Compiler::resolve_subtarget is a pure function of the triple and the two
    // requests, so this answer is the same one Compiler::compute_module_keys folded into the cache key
    // long before the backend existed. Keeping a copy here would be a second place for the two to drift,
    // and the drift is precisely the unsound-cache case: two subtargets served each other's objects
    Compiler::Subtarget subtarget;
    std::string error;

    if (!Compiler::resolve_subtarget(
            _ctx.target_triple, _ctx.options.target_cpu, _ctx.options.target_features,
            subtarget, error)) {
        // the driver validated these off the same function before a single file was parsed, so a
        // refusal here is a disagreement between two calls that cannot disagree
        throw Compiler::InternalCompilerException(error);
    }

    return subtarget;
}

void Backend::init_target()
{
    Compiler::ensure_native_target_registered();

    _ctx.target_triple = llvm::sys::getDefaultTargetTriple();

    std::string error;
    auto *target = llvm::TargetRegistry::lookupTarget(_ctx.target_triple, error);
    if (!target) {
        throw Compiler::InternalCompilerException(fmt::format(
            "Could not resolve the host target '{}': {}", _ctx.target_triple, error));
    }

    // **the subtarget is not a detail of this line.** every cost model in the compiler reads the machine
    // built here - both PassBuilder pipelines through their TargetTransformInfo, and instruction
    // selection in emit_object - so `generic` was the vectorizer answering for a CPU nobody runs on, at
    // about 1.8x on any loop whose throughput it decides. Compiler::baseline_subtarget_for owns what the
    // default is, and it is a table rather than a policy in here
    const Compiler::Subtarget sub = subtarget();

    // **and the machine's own optimization level, which is not the IR pipeline's.** createTargetMachine
    // defaults to CodeGenOptLevel::Default - O2 - so instruction selection, machine scheduling, stack
    // slot colouring and the peephole passes all ran even under `--no-optimize`, which turns off only
    // Backend::optimize_unit. That is invisible in an IR dump and fatal to a debugger: an unoptimized
    // body still came out with its stores merged and its frame folded into a post-indexed `stp`, so
    // every local's DWARF location named a stack slot the function had already given back, and
    // `frame variable` printed whatever was there.
    //
    // read off no_optimize rather than off debug_info, because "do not optimize" is what the flag says
    // and a machine level that contradicts it is a second answer. `-g` reaches it by implying that flag,
    // which is settled once in resolve_options
    const llvm::CodeGenOptLevel opt_level =
        _ctx.options.no_optimize ? llvm::CodeGenOptLevel::None : llvm::CodeGenOptLevel::Default;

    llvm::TargetOptions opt;
    _target_machine.reset(target->createTargetMachine(
        _ctx.target_triple, sub.cpu, sub.features, opt, llvm::Reloc::PIC_, std::nullopt, opt_level));
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
    const std::string &executable_name,
    const std::vector<std::filesystem::path> &objects,
    const std::vector<std::filesystem::path> &link_objects,
    const std::vector<std::string> &link_words
)
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

    // the build's own prebuilt objects sit with ours, ahead of every library: a `-l` resolves against the
    // undefined symbols the linker has seen so far, and an object added after them contributes none
    append_objects(command, link_objects);

    command.push_back("-lSystem");
    command.push_back("-syslibroot");
    command.push_back(sdk_root);
    command.push_back("-arch");
    command.push_back(arch);

    command.insert(command.end(), link_words.begin(), link_words.end());

    return command;
#else
    (void)executable_name;
    (void)objects;
    (void)link_objects;
    (void)link_words;
    return std::nullopt;
#endif
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

    // the directory is the driver's - it prepared one before it decided to emit here at all. Creating one
    // on the way past would be a second answer to where a build artifact goes, in the layer furthest from
    // the question
    std::error_code ec;

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
    const std::string &executable_name,
    const std::vector<std::filesystem::path> &objects,
    const std::vector<Compiler::LinkRequirement> &link
)
{
    if (objects.empty()) {
        llvm::errs() << "Error: nothing to link\n";
        return false;
    }

    // **rendered once, for both spellings below.** An `object:` becomes another object file and everything
    // else becomes words after them, and which is which is not a decision either call site repeats
    std::vector<std::filesystem::path> link_objects;
    std::vector<std::string> link_words;
    Compiler::partition_link_requirements(link, link_objects, link_words);

    if (const auto command = host_linker_command(executable_name, objects, link_objects, link_words)) {
        if (Compiler::run_tool(command.value())) {
            gen_debug_symbols(executable_name);
            return true;
        }

        // not a hard failure: the flags above are a guess about the host, and clang below knows better.
        // A linker error in the *program* will be reported by clang in a moment anyway
        llvm::errs() << "Note: the system linker failed, retrying through the clang driver\n";
    }

    std::vector<std::string> fallback = { "clang", "-o", executable_name };
    append_objects(fallback, objects);
    append_objects(fallback, link_objects);
    fallback.insert(fallback.end(), link_words.begin(), link_words.end());

    // **no message of its own.** Whichever tool ran has already said what went wrong, on the stderr it
    // inherited; naming which module asked for each requirement is the driver's to render, because that
    // sentence needs the DiagnosticRenderer and this layer has neither it nor a reason to grow one
    if (!Compiler::run_tool(fallback)) {
        return false;
    }

    gen_debug_symbols(executable_name);

    return true;
}

void Backend::gen_debug_symbols(const std::string &executable_name)
{
#if defined(__APPLE__)
    if (!_ctx.options.emitting_debug_info()) {
        return;
    }

    // **the debug info is not in the executable on Mach-O.** `ld` writes a *debug map* instead - one
    // N_OSO stab per object, naming it by absolute path and mtime - and lldb reads the DWARF back out of
    // those objects at debug time. Which works right up until the module cache is cleaned, the tree is
    // moved or the binary is copied to another machine, and then reports "no debug symbols" with nothing
    // saying why. dsymutil is what folds them into a self-contained <exe>.dSYM.
    //
    // here rather than in make_exec, so the whole-program and per-module paths both get it - and
    // explicitly rather than left to the clang driver, because the preferred path is a direct `ld`
    // invocation that will never run it, which would make debuggability depend on whether
    // `xcrun --show-sdk-path` happened to answer
    //
    // **best effort**, exactly as host_linker_command is: a toolchain without dsymutil still produces a
    // binary that runs, and one that is debuggable for as long as its objects stay put
    if (!Compiler::run_tool({ "dsymutil", executable_name })) {
        llvm::errs() << "Note: dsymutil failed or is unavailable - the executable is debuggable only "
                        "while its object files remain where they were linked from\n";
    }
#else
    // ELF links DWARF straight into the executable, so there is nothing to collect
    (void)executable_name;
#endif
}

bool Backend::make_exec(
    const std::string &executable_name,
    const std::filesystem::path &object_path,
    const std::vector<Compiler::LinkRequirement> &link
)
{
    Compiler::LLVM::CmpUnit *main_cmp_unit = _ctx.main_cmp_unit();
    if (!main_cmp_unit) {
        llvm::errs() << "No main module found to emit\n";
        return false;
    }

    if (!emit_object(*main_cmp_unit, object_path)) {
        return false;
    }

    return link_executable(executable_name, { object_path }, link);
}

// guards the module, builds the four analysis managers and cross-registers the proxies, then runs
// whatever pipeline `build` filled in. Every module-pass entry point below goes through here, so how
// analyses are registered is decided once - two spellings of it drift silently, and a pass that then
// asks for an analysis nobody registered aborts inside LLVM rather than here.
//
// file-local rather than a member: llvm::ModulePassManager is a template alias, so a declaration in
// Backend.h would drag the whole pass infrastructure into every translation unit that includes it -
// which is also why the TargetMachine arrives as a parameter rather than being read off `this`
//
// **the machine is what makes the pipeline know what it is compiling for.** a default-constructed
// PassBuilder gets a no-op TargetTransformInfo, whose answer to "how wide is a vector register" is
// *one* - so LoopVectorize and SLPVectorize are in every pipeline below and can never fire, and the
// inliner and unroller cost models are generic guesses rather than this target's. It is not a
// tuning knob: without it an `int32` reduction over an `array<int32>` emits a four-instruction
// scalar loop at `-O`, which is what `entry_alloca`'s careful slot placement was buying nothing for
static void run_module_passes(
    llvm::Module &module,
    llvm::TargetMachine *target_machine,
    const std::function<void(llvm::PassBuilder &, llvm::ModulePassManager &)> &build
)
{
    llvm::PassBuilder passBuilder(target_machine);
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

    run_module_passes(*_ctx.current_module(), _target_machine.get(), [](llvm::PassBuilder &passBuilder, llvm::ModulePassManager &modulePM) {
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
    run_module_passes(*cmp_unit.llvm_module, _target_machine.get(), [](llvm::PassBuilder &passBuilder, llvm::ModulePassManager &modulePM) {
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

    // the machine is inert for these two - neither Internalize nor GlobalDCE consults a cost model - and
    // is passed anyway so there is one spelling of "how a pipeline is built" rather than two
    run_module_passes(*_ctx.current_module(), _target_machine.get(), [](llvm::PassBuilder &, llvm::ModulePassManager &modulePM) {
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
