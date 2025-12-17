#include "Compiler/TargetSubtarget.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

#include <algorithm>
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

    // **the baseline table.** one row, and the narrowness is deliberate: a row is a claim that every
    // machine running that triple has that CPU, and the only one this compiler has measured is the
    // Apple Silicon Mac the vectorizer numbers were taken on. Anything unlisted keeps `generic`, which
    // is what it had before this table existed - so adding a platform is adding a row and cannot
    // regress one that is not here.
    //
    // it matches what rustc defaults to for the same triple, which is what makes
    // `rustc -O -C target-cpu=generic` a control column rather than a coincidence
    std::string baseline_cpu_of(const llvm::Triple &triple)
    {
        // every Apple Silicon Mac is an M1 or later - the triple *is* the guarantee here, because
        // aarch64 Darwin has no earlier hardware to be compatible with
        if (triple.isOSDarwin() && triple.isAArch64()) {
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
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        return true;
    }();

    (void)registered;
}

Subtarget baseline_subtarget_for(const std::string &triple)
{
    Subtarget subtarget;
    subtarget.cpu = baseline_cpu_of(llvm::Triple(triple));

    // no baseline row carries features: a feature the CPU already implies does not need saying, and
    // one it does not imply is not a baseline
    return subtarget;
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

    // **checked against the real target, because LLVM does not refuse.** `createTargetMachine` with a
    // CPU it does not know prints "'x' is not a recognized processor for this target (ignoring
    // processor)" to stderr and builds the object for something else - a warning in a stream nobody
    // reads and a silently different binary
    ensure_native_target_registered();

    std::string lookup_error;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, lookup_error);
    if (target == nullptr) {
        out_error = fmt::format("cannot resolve the target '{}': {}", triple, lookup_error);
        return false;
    }

    // built with **no** CPU and asked about ours afterwards. Handing it the name is how you get LLVM's
    // own answer, which is the warning this exists to replace - printed to stderr underneath whatever
    // else is on it, and followed by an object built for something the person did not ask for
    const std::unique_ptr<llvm::MCSubtargetInfo> info(
        target->createMCSubtargetInfo(triple, "", "")
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

};
