#ifndef CODEGENTARGET_H
#define CODEGENTARGET_H

#pragma once

#include <cassert>
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

        bool is_wasm() const {
            return _os == TargetOs::t_wasi;
        }

        bool is_windows() const {
            return _os == TargetOs::t_windows;
        }

        bool is_darwin() const {
            return _os == TargetOs::t_darwin || _os == TargetOs::t_ios;
        }

        // `main`, or `__main_argc_argv` on wasi. a library has none
        std::optional<const char *> entry_symbol() const;

        ReservedExportNames reserved_export_names() const;

        bool is_library() const {
            return _exec_model == ExecModel::t_library;
        }

        bool is_native_library() const {
            return is_library() && !is_wasm();
        }

        // 3 on native (argc, argv, envp), 2 on wasi (argc, argv)
        unsigned entry_arg_count() const;

        bool entry_receives_envp() const {
            return entry_arg_count() == 3;
        }

        EnvironSource environ_source() const;
        StdioUnbuffering stdio_unbuffering() const;

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
        unsigned _pointer_bytes = 8;
    };

    // why this program cannot be a loadable library. empty when it can
    std::optional<const char *> library_refusal(const TargetFacts &facts);

    // cache-key spelling of exec_model. every kind is its own key
    inline const char *exec_model_environment(const CodegenTarget &row)
    {
        switch (row.exec_model()) {
            case ExecModel::t_command: return "cmd";
            case ExecModel::t_library: return row.is_wasm() ? "reactor" : "lib";
        }

        assert(false && "unhandled ExecModel in exec_model_environment");
        return "cmd";
    }

    // `--target-arch` as TargetFacts::resolve should see it. wasi is wasm32 on every subcommand
    bool facts_architecture(
        bool ios_device,
        const std::string &arch_override,
        const std::string &target_os,
        std::string &out_arch,
        std::string &out_error
    );

    // what `build` emits. `run` / `test` / `clean` / `lsp` keep the host row
    bool resolve_codegen_target(
        const TargetFacts &facts,
        bool ios_device,
        const std::string &arch_override,
        CodegenTarget &out_target,
        std::string &out_error);
};

#endif
