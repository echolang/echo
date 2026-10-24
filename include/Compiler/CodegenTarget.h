#ifndef CODEGENTARGET_H
#define CODEGENTARGET_H

#pragma once

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Compiler
{
    struct TargetFacts;

    // the WASI row's LLVM triple. one spelling, so WasiSdk's resource-dir lookup
    // and resolve_codegen_target cannot drift
    inline constexpr const char *k_wasi_triple = "wasm32-unknown-wasip1";

    // LLVM triple and sysroot this invocation emits. not TargetFacts: that is `#[if:]`
    enum class ExecModel
    {
        t_command,
        t_library,
    };

    // the OS a row emits for. read off the triple when the row is built
    enum class TargetOs
    {
        t_darwin,
        t_ios,
        t_linux,
        t_windows,
        t_wasi,
        t_other,
    };

    // where a program reads its environment when no entry captured it
    enum class EnvironSource
    {
        t_libc_environ,
        t_ns_get_environ,
        t_wasilibc_get_environ,
        t_none,
    };

    enum class StdioUnbuffering
    {
        t_none,
        t_ucrt_stdout_stderr,
        t_wasi_libc_stdout,
    };

    // where the per-process hash seed comes from. none of these can block or fail once a process is running
    enum class EntropySource
    {
        // `arc4random_buf`, which has no failure path at all
        t_arc4random,
        // `getentropy`, glibc 2.25+, musl, Android 28+, and wasi-libc over `random_get`
        t_getentropy,
        // UCRT `rand_s`, four bytes a call
        t_rand_s,
    };

    // how a native lib looks up a symbol in the host image. `dlopen(NULL)` from a ctor can deadlock
    enum class HostSymbolLookup
    {
        t_dlsym,
        t_get_proc_address,
    };

    struct ReservedExportNames
    {
        std::span<const char *const> entry;
        std::span<const char *const> runtime;
        std::string_view runtime_prefix;
        std::span<const char *const> wasm_crt;
    };

    // LLVM triple and sysroot this invocation emits
    class CodegenTarget
    {
    public:
        CodegenTarget();
        static CodegenTarget wasi();
        static CodegenTarget for_triple(std::string triple);
        static CodegenTarget ios_simulator(const std::string &arch);
        static CodegenTarget ios_device();
        CodegenTarget for_program(ExecModel model) const;

        // the LLVM triple objects are emitted for. empty means the host
        const std::string &triple() const {
            return _triple;
        }

        // `xcrun --sdk` name. empty means the Mac SDK (`darwin_sdk_root`) or none.
        // `iphonesimulator` / `iphoneos` are the two iOS rows
        const std::string &apple_sdk() const {
            return _apple_sdk;
        }

        ExecModel exec_model() const {
            return _exec_model;
        }

        TargetOs os() const {
            return _os;
        }

        bool is_cross() const {
            return !_triple.empty();
        }

        // empty falls back to the host
        std::string effective_triple() const;

        unsigned pointer_size() const {
            return _pointer_bytes;
        }

        // wasm ISA (wasm32 / wasm64), read off the triple. PIC, bitcode snapshots,
        // `.wasm` suffix, and LLVM `wasm-export-name` follow this. WASI ABI facts
        // go through `is_wasi()` / `os()`
        bool is_wasm() const {
            return _wasm_arch;
        }

        bool is_wasi() const {
            return _os == TargetOs::t_wasi;
        }

        bool is_windows() const {
            return _os == TargetOs::t_windows;
        }

        bool is_darwin() const {
            return _os == TargetOs::t_darwin || _os == TargetOs::t_ios;
        }

        std::string_view line_ending() const {
            return is_windows() ? "\r\n" : "\n";
        }

        // `main`, or `__main_argc_argv` on wasi. a library has none
        std::optional<const char *> entry_symbol() const;

        ReservedExportNames reserved_export_names() const;

        bool is_library() const {
            return _exec_model == ExecModel::t_library;
        }

        // a loadable native image. wasm has no dlclose, so a wasm library is not one
        bool is_native_library() const {
            return is_library() && has_unload();
        }

        // native extern-interface tables. wasm has none; TypeChecker asks this
        // rather than a `bool wasm` threaded through AST
        bool exports_abi_tables() const {
            return !is_wasm();
        }

        // 3 on native (argc, argv, envp), 2 on wasi (argc, argv)
        unsigned entry_arg_count() const;

        bool entry_receives_envp() const {
            return entry_arg_count() == 3;
        }

        EnvironSource environ_source() const;
        StdioUnbuffering stdio_unbuffering() const;
        EntropySource entropy_source() const;
        HostSymbolLookup host_symbol_lookup() const;

        // `RTLD_DEFAULT` as an integer `dlsym` takes. unused on `t_get_proc_address`
        int64_t rtld_default() const;

        // false for every wasm ISA, not only wasi. a switch on OS would say yes for a
        // wasm triple LLVM still classifies as linux
        bool has_unload() const {
            return !is_wasm();
        }

        bool uses_pic() const;

        bool c_objects_use_fpic() const {
            return uses_pic() && !is_windows();
        }

        bool snapshots_bitcode() const;

        const char *output_extension() const;
        const char *library_suffix() const;
        std::string library_file_name(const std::string &stem) const;
        std::filesystem::path output_path(std::filesystem::path path) const;

        bool has_os_threads() const {
            return !is_wasm();
        }

    private:
        CodegenTarget(std::string triple, std::string apple_sdk);

        std::string _triple;
        std::string _apple_sdk;
        ExecModel _exec_model = ExecModel::t_command;
        TargetOs _os = TargetOs::t_other;
        bool _wasm_arch = false;
        unsigned _pointer_bytes = 8;
    };

    // why this program cannot be a loadable library. empty when it can
    std::optional<const char *> library_refusal(const TargetFacts &facts);

    // cache-key spelling of exec_model. every kind is its own key
    inline const char *exec_model_environment(const CodegenTarget &row)
    {
        switch (row.exec_model()) {
            case ExecModel::t_command: return "cmd";
            case ExecModel::t_library: return row.is_wasi() ? "reactor" : "lib";
        }

        assert(false && "unhandled ExecModel in exec_model_environment");
        return "cmd";
    }

    // what this invocation emits, from already-settled facts plus `--ios-device`.
    // arch implications (wasi is wasm32, ios-device is arm64) live on TargetFacts::resolve.
    // `build` and analysis paths (lsp, emit-only OS) fill a row; `run` / `test` keep the
    // host row so `--target-os ios` stays a facts-only look at another platform's branch
    bool resolve_codegen_target(
        const TargetFacts &facts,
        bool ios_device,
        CodegenTarget &out_target,
        std::string &out_error);
};

#endif
