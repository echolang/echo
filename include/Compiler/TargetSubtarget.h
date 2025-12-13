#ifndef TARGETSUBTARGET_H
#define TARGETSUBTARGET_H

#pragma once

#include <string>
#include <vector>

namespace Compiler
{
    // **the sole answer to "which CPU is the backend compiling for".**
    //
    // a CPU name and a feature string, and between them everything llvm::TargetMachine needs beyond the
    // triple. every cost model in the compiler is downstream of this: both PassBuilder pipelines take
    // their TargetTransformInfo from the machine built out of it, and so does instruction selection - so
    // a subtarget nobody is running on is a vectorizer answering for a machine that does not exist.
    //
    // deliberately **not** on Compiler::TargetFacts, close as the two read. That one answers what a
    // `#[if: ...]` condition can see and says in its own comment that it does not cross-compile; this
    // one changes what is emitted. Folding them would make `--target-os` - which exists so a test can
    // assert what another platform's branch does - start choosing instruction sets.
    struct Subtarget
    {
        // never empty: the resolver's floor is "generic", which is what LLVM itself means by "the
        // baseline this triple guarantees". an empty string is a *different* thing to LLVM's JIT than
        // to its TargetMachine, and one of them auto-detects the host
        std::string cpu;

        // `+feature,-feature,...`, empty for every baseline row. a feature the CPU already implies does
        // not need saying, so this is only ever what a person asked for on top
        std::string features;
    };

    // **LLVM's native target, registered.** Idempotent, and here rather than left implicit in
    // Backend::init_target because nothing may be *asked* about a target before it is in the registry -
    // and the CPU a person wrote has to be checked at the command line, which is long before the
    // backend exists. One owner, so the two cannot register different sets
    void ensure_native_target_registered();

    // **the platform baseline for a triple** - the default, and neither `generic` nor the host.
    //
    // one row per triple family, and anything unlisted falls back to `generic`, so teaching this
    // compiler about a platform is adding a row rather than changing a policy. The rule the rows are
    // written under: a row may only name a CPU that *every* machine running that triple has, because
    // `echoc build` hands somebody a binary and an illegal instruction is not a diagnostic.
    Subtarget baseline_subtarget_for(const std::string &triple);

    // what a person wrote, turned into what the backend gets. Both requests may be empty, which means
    // "the baseline"; a cpu request of `native` is the explicit opt-in to this machine.
    //
    // **a pure function of its arguments**, which is the whole reason it is a free function rather than
    // state on anything: Backend::init_target and Compiler::compute_module_keys both have to reach the
    // identical answer, and they run at opposite ends of a compile. A stored resolution would be a
    // second place for them to disagree, and the disagreement is the unsound-cache case.
    //
    // false with a sentence in `out_error` when the request names a CPU this target does not have -
    // LLVM's own answer to that is a warning on stderr and an object quietly built for something else
    bool resolve_subtarget(
        const std::string &triple,
        const std::string &cpu_request,
        const std::string &features_request,
        Subtarget &out_subtarget,
        std::string &out_error);

    // the feature string as a list, because llvm::EngineBuilder takes one that way while
    // llvm::Target::createTargetMachine takes the comma-joined form. Here rather than at the JIT,
    // because the *format* of that field is this type's question and a second split would be a second
    // reading of it - empty in, empty out, which is what keeps an untouched build untouched
    std::vector<std::string> split_target_features(const std::string &features);

    // `cpu=apple-m1;features=;` - what the module cache folds, this type's own question the way
    // TargetFacts::cache_signature() is. A field added above without a line here would silently share a
    // key with a build that did not have it, which is the one cache failure with no diagnostic
    std::string subtarget_signature(const Subtarget &subtarget);
};

#endif
