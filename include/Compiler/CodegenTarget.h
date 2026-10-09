#ifndef CODEGENTARGET_H
#define CODEGENTARGET_H

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Compiler
{
    struct TargetFacts;

    // the WASI row's LLVM triple. one spelling, so WasiSdk's resource-dir lookup
    // and resolve_codegen_target cannot drift
    inline constexpr const char *k_wasi_triple = "wasm32-unknown-wasip1";

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
    // rather than another flag recovered from a triple string later. the min version lives
    // in the triple (`arm64-apple-ios15.0`), so LLVM's object and clang's `-target` cannot
    // pin different iOS versions.

    // how a program is entered. a column of the row, like apple_sdk: per *program*,
    // not per invocation, because one module can declare an exe (command) and a lib
    // and `echoc build` produces both. a library has no entry; its surface is its
    // `#[export]`s. on wasm that is a reactor (`crt1-reactor.o`, `_initialize`);
    // on the host it is a shared library
    enum class ExecModel
    {
        t_command,
        t_library,
    };

    struct CodegenTarget
    {
        // LLVM triple objects are emitted for. empty means the host
        std::string triple;

        // `xcrun --sdk` name. empty means the Mac SDK (`darwin_sdk_root`) or none.
        // `iphonesimulator` / `iphoneos` are the two iOS rows
        std::string apple_sdk;

        ExecModel exec_model = ExecModel::t_command;

        // columns of the row, set in resolve_codegen_target. empty/host is not wasm
        // and 8-byte pointers; the wasi arm sets both
        bool wasm = false;
        unsigned pointer_bytes = 8;

        bool is_cross() const {
            return !triple.empty();
        }

        // the triple Backend::init_target and fold_target_environment fold.
        // empty falls back to the host, so a default-constructed CompilerOptions
        // and an invocation with no `--target-os` agree
        std::string effective_triple() const;

        // width of a pointer on this row, in bytes. wasm32 is 4; every host this
        // compiler has shipped is 8. parse/typecheck installs this via
        // AST::PointerSizeScope; codegen usize is size_int_ty()
        unsigned pointer_size() const {
            return pointer_bytes;
        }

        // wasi is the other real cross on every host. a column, so LLVMCompiler,
        // the linker, PIC, and the entry symbol do not each invent `Triple::isWasm()`
        bool is_wasm() const {
            return wasm;
        }

        // the C symbol the entry module is emitted under. host and JIT are
        // `main`; wasi-libc's crt looks up `__main_argc_argv`. a library has
        // none: its surface is its exports. on wasm, crt1-reactor.o provides
        // `_initialize`
        std::optional<const char *> entry_symbol() const;

        bool is_library() const {
            return exec_model == ExecModel::t_library;
        }

        // a host shared library, so hidden-except-export and `clang -shared`.
        // a wasm library is a reactor and uses the export section instead
        bool is_native_library() const {
            return is_library() && !is_wasm();
        }

        // 3 on native (argc, argv, envp), 2 on wasi (argc, argv). envp is
        // stored as null on the two-argument row
        unsigned entry_arg_count() const;

        // PIC is the native loadable-object rule. a WASI command module is a
        // static relocatable; PIC is a different wasm ABI
        bool uses_pic() const;

        // wasm objects fail the same-process bitcode round-trip (`Invalid
        // record`) the way Windows does, so they stay in-memory
        bool snapshots_bitcode() const;

        // appended when `-o` has no extension. empty on native (Windows `.exe`
        // is the host fallback in output_path, not a row fact)
        const char *output_extension() const;

        // the path a suffix-less `-o` or target name becomes. the row's
        // output_extension is the only suffix; Windows `.exe` applies only
        // when that is empty. a path that already has an extension is left
        std::filesystem::path output_path(std::filesystem::path path) const;

        // WASI preview 1 has no OS threads. the once helper and the static
        // teardown chain are then ordinary loads and stores
        bool has_os_threads() const;
    };

    // `--target-arch` as TargetFacts::resolve should see it. `--ios-device` with
    // no arch override is arm64, so a condition cannot still see the Intel host
    // while the row emits a phone. `emitting` is `build`: wasi then defaults to
    // wasm32 the same way, and `run --target-os wasi` keeps the host arch
    std::string facts_architecture(
        bool ios_device,
        const std::string &arch_override,
        const std::string &target_os = {},
        bool emitting = false
    );

    // what this invocation emits objects for, given the facts a condition can see
    // and the `--ios-device` request. asked only by `build`: `run` / `test` / `clean`
    // / `lsp` never call it, so an empty CodegenTarget (the host) is what they emit.
    //
    // Darwin `echoc build --target-os ios` is a real cross-compile (simulator by
    // default, iPhoneOS with `--ios-device`). `echoc build --target-os wasi` is
    // the other real cross: `wasm32-unknown-wasip1` on every host. every other
    // override stays the host triple: there is no Linux sysroot on a Mac.
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
