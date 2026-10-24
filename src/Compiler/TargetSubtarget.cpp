#include "Compiler/TargetSubtarget.h"
#include "Compiler/CodegenTarget.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

namespace Compiler
{

namespace
{
    // the floor, and the only CPU name spelled in more than one place - so it is spelled once
    constexpr const char *k_generic_cpu = "generic";

    // what a person writes to ask for this machine rather than the baseline
    constexpr const char *k_native_cpu = "native";

    // a row is a claim that every machine of that triple has that CPU. unlisted stays `generic`
    std::string baseline_cpu_of(const llvm::Triple &triple)
    {
        // every Apple Silicon Mac is an M1 or later - the triple *is* the guarantee here, because
        // aarch64 macOS has no earlier hardware to be compatible with. isMacOSX, not isOSDarwin:
        // iOS is Darwin to LLVM, and an iPhone is not an M1. unlisted platforms stay generic
        if (triple.isMacOSX() && triple.isAArch64()) {
            return "apple-m1";
        }

        // x86_64 deliberately stays at the floor. `x86-64-v2` would be the tempting row and it is the
        // one that hands somebody a binary their machine cannot run, silently, for a gain nothing here
        // has measured
        return k_generic_cpu;
    }

    // `+a,+b,-c` out of what the host reports. Sorted by StringMap iteration order, which is *not*
    // stable across runs - so the entries are collected and sorted, because this string reaches the
    // module cache key and a key that depends on a hash table's layout misses for no reason
    std::string host_feature_string()
    {
        std::vector<std::string> entries;

        for (const auto &feature : llvm::sys::getHostCPUFeatures()) {
            entries.push_back(
                fmt::format("{}{}", feature.second ? "+" : "-", feature.first().str()));
        }

        std::sort(entries.begin(), entries.end());

        std::string result;
        for (const std::string &entry : entries) {
            if (!result.empty()) {
                result += ",";
            }
            result += entry;
        }

        return result;
    }
};

void ensure_native_target_registered()
{
    // a function-local static rather than the raw calls: llvm's own registration is idempotent, but
    // "has this happened yet" is a question two callers would otherwise each answer their own way
    static const bool registered = [] {
        // LLVM's default handler fprintf()s and abort()s. on a Windows /MT
        // echoc linked against /MD LLVM that mix is STATUS_STACK_BUFFER_OVERRUN
        // (0xC0000409) and GHA logs only "Process completed with exit code 1".
        // here, not main: --version must not touch LLVM, and this is the first
        // call every compile makes. _Exit so abort() cannot swallow the sentence
        llvm::install_fatal_error_handler([](void *, const char *reason, bool) {
            llvm::errs() << "LLVM fatal error: " << (reason != nullptr ? reason : "") << "\n";
            llvm::errs().flush();
            std::_Exit(1);
        });

        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
#if defined(__APPLE__)
        // iOS rows are AArch64 (device) or the host arch (simulator). native is one of those;
        // the other has to be in the registry too or lookupTarget refuses a triple
        // CodegenTarget can name. initializing the native one twice is a no-op
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeX86AsmParser();
#endif
        return true;
    }();

    (void)registered;
}

void ensure_targets_for_triple(const std::string &triple)
{
    ensure_native_target_registered();

    // wasi is a real cross on every host. CodegenTarget names
    // `wasm32-unknown-wasip1`; without these, lookupTarget refuses it.
    // a host compile does not need them, and initializing them in a
    // Windows /MT release binary is the first extra LLVM work `build` did
    if (!llvm::Triple(triple).isWasm()) {
        return;
    }

    static const bool wasm_registered = [] {
        LLVMInitializeWebAssemblyTargetInfo();
        LLVMInitializeWebAssemblyTarget();
        LLVMInitializeWebAssemblyTargetMC();
        LLVMInitializeWebAssemblyAsmPrinter();
        LLVMInitializeWebAssemblyAsmParser();
        return true;
    }();

    (void)wasm_registered;
}

Subtarget baseline_subtarget_for(const std::string &triple)
{
    Subtarget subtarget;
    const llvm::Triple parsed(triple);
    subtarget.cpu = baseline_cpu_of(parsed);

    // wasm SIMD is 16 bytes, which is the language's vector cap. without +simd128
    // LLVM scalarizes every `simd<T, N>` on that row
    if (parsed.isWasm()) {
        subtarget.features = "+simd128";
    }

    return subtarget;
}

std::string llvm_cpu_name(const std::string &triple, const std::string &cpu)
{
    if (cpu != k_generic_cpu) {
        return cpu;
    }

    const llvm::Triple parsed(triple);
    if (parsed.getArch() == llvm::Triple::x86_64) {
        return "x86-64";
    }
    if (parsed.getArch() == llvm::Triple::x86) {
        return "i686";
    }

    return cpu;
}

bool resolve_subtarget(
    const std::string &triple,
    const std::string &cpu_request,
    const std::string &features_request,
    Subtarget &out_subtarget,
    std::string &out_error)
{
    out_subtarget = baseline_subtarget_for(triple);

    if (cpu_request == k_native_cpu) {
        // **the one place the host may decide anything**, and only because somebody asked for it by
        // name. The default must never reach here: `getHostCPUName()` is `-march=native`, which is an
        // object that may not run on the machine next to it, with an illegal instruction rather than a
        // diagnostic - and `echoc build` hands somebody a binary
        const llvm::Triple host(llvm::sys::getDefaultTargetTriple());
        const llvm::Triple target(triple);

        if (target.getArch() != host.getArch() || target.getOS() != host.getOS()) {
            out_error = fmt::format(
                "--target-cpu native is this machine; the target '{}' is not", triple);
            return false;
        }

        out_subtarget.cpu = llvm::sys::getHostCPUName().str();
        out_subtarget.features = host_feature_string();
    }
    else if (!cpu_request.empty()) {
        out_subtarget.cpu = cpu_request;
    }

    if (!features_request.empty()) {
        // written *after* `native` rather than merged into it, so an explicit `--target-features` is
        // the last word - which is what a person asking for one means
        out_subtarget.features = features_request;
    }

    // generic is this compiler's floor, not an LLVM CPU name. x86's MC layer
    // prints "'generic' is not a recognized processor" and continues; clang
    // wants `x86-64` for the same floor (append_codegen_cc_cpu_args omits the
    // flag). on a Windows /MT echoc linked against /MD LLVM that fprintf is
    // STATUS_STACK_BUFFER_OVERRUN (0xC0000409) and GHA logs only exit 1.
    // an empty CPU string is worse: LLVM reads it as the host CPU.
    // Backend/TargetMachine still call ensure_targets_for_triple
    if (out_subtarget.cpu == k_generic_cpu) {
        return true;
    }

    // **checked against the real target, because LLVM does not refuse.** `createTargetMachine` with a
    // CPU it does not know prints "'x' is not a recognized processor for this target (ignoring
    // processor)" to stderr and builds the object for something else - a warning in a stream nobody
    // reads and a silently different binary
    ensure_targets_for_triple(triple);

    std::string lookup_error;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, lookup_error);
    if (target == nullptr) {
        out_error = fmt::format("cannot resolve the target '{}': {}", triple, lookup_error);
        return false;
    }

    const std::unique_ptr<llvm::MCSubtargetInfo> info(
        target->createMCSubtargetInfo(triple, out_subtarget.cpu, "")
    );

    if (!info || !info->isCPUStringValid(out_subtarget.cpu)) {
        out_error = fmt::format(
            "unknown --target-cpu '{}' for target '{}'. Write 'native' for this machine, or leave it "
            "unset for the platform baseline ('{}')",
            out_subtarget.cpu, triple, baseline_subtarget_for(triple).cpu);
        return false;
    }

    return true;
}

std::vector<std::string> split_target_features(const std::string &features)
{
    std::vector<std::string> out;
    size_t start = 0;

    while (start < features.size()) {
        const size_t comma = features.find(',', start);
        const size_t end = comma == std::string::npos ? features.size() : comma;

        if (end > start) {
            out.push_back(features.substr(start, end - start));
        }

        start = end + 1;
    }

    return out;
}

std::string subtarget_signature(const Subtarget &subtarget)
{
    return "cpu=" + subtarget.cpu + ";features=" + subtarget.features + ";";
}

bool append_codegen_cc_cpu_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    const std::string &cpu_request,
    const std::string &features_request,
    std::string &out_error)
{
    Subtarget sub;
    if (!resolve_subtarget(
            target.effective_triple(),
            cpu_request,
            features_request,
            sub,
            out_error)) {
        return false;
    }

    // clang's `-target-cpu` does not accept LLVM's floor name `generic` on x86
    // (`x86-64` is the spelling). omitting the flag is the same floor: clang
    // already compiles for the triple's baseline. a named CPU (apple-m1, a
    // written `--target-cpu`) still has to be said
    if (!sub.cpu.empty() && sub.cpu != k_generic_cpu) {
        argv.push_back("-Xclang");
        argv.push_back("-target-cpu");
        argv.push_back("-Xclang");
        argv.push_back(sub.cpu);
    }

    for (const std::string &feature : split_target_features(sub.features)) {
        argv.push_back("-Xclang");
        argv.push_back("-target-feature");
        argv.push_back("-Xclang");
        argv.push_back(feature);
    }

    return true;
}

};
