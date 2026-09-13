#ifndef CODEGENTARGET_H
#define CODEGENTARGET_H

#pragma once

#include <string>
#include <vector>

namespace Compiler
{
    struct TargetFacts;

    // **the sole answer to "what LLVM triple and sysroot this invocation emits".**
    //
    // deliberately **not** on Compiler::TargetFacts. That one answers what a `#[if:]` can see
    // and does not cross-compile; this one changes what is emitted. Folding them is what
    // would make `--target-os linux` start choosing instruction sets.
    //
    // empty `triple` is the host, which is also what a default-constructed CompilerOptions
    // means - the same rule CompilerOptions already states for target_cpu. Backend and the
    // C build both read it off CompilerOptions, so a C object and an Echo object cannot
    // disagree about the triple or the SDK.
    //
    // a **row**, not a bool: tvOS or the Android NDK is another row in resolve_codegen_target
    // rather than another flag recovered from a triple string later
    struct CodegenTarget
    {
        // LLVM triple objects are emitted for. empty means the host
        std::string triple;

        // `xcrun --sdk` name. empty means the Mac SDK (`darwin_sdk_root`) or none.
        // `iphonesimulator` / `iphoneos` are the two iOS rows
        std::string apple_sdk;

        // clang min-version flag, empty when the host SDK has no such pin
        std::string min_version_flag;

        bool is_cross() const {
            return !triple.empty();
        }

        // the triple Backend::init_target and fold_target_environment fold.
        // empty falls back to the host, so a default-constructed CompilerOptions
        // and an invocation with no `--target-os` agree
        std::string effective_triple() const;
    };

    // `-isysroot` / `-target` / min-version for this row. empty apple_sdk is
    // append_darwin_sdk_args
    void append_apple_target_args(std::vector<std::string> &argv, const CodegenTarget &target);

    // what this invocation emits objects for, given the facts a condition can see
    // and the `--ios-device` request. asked only by `build`: `run` / `test` / `clean`
    // / `lsp` never call it, so an empty CodegenTarget (the host) is what they emit.
    //
    // Darwin `echoc build --target-os ios` is a real cross-compile (simulator by
    // default, iPhoneOS with `--ios-device`). every other override stays the host
    // triple: there is no Linux sysroot on a Mac.
    //
    // false with a sentence when `--ios-device` cannot retarget: without
    // `--target-os ios`, off Darwin, or with `--target-arch` other than arm64
    bool resolve_codegen_target(
        const TargetFacts &facts,
        bool ios_device,
        const std::string &arch_override,
        CodegenTarget &out_target,
        std::string &out_error);
};

#endif
