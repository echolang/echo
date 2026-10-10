#include "Compiler/CodegenTarget.h"

#include "Compiler/TargetFacts.h"

#include "eco.h"

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

#include <filesystem>
#include <optional>
#include <span>

namespace Compiler
{

static constexpr const char *k_ios_min_version = "15.0";
static constexpr const char *k_ios_device_arch = "arm64";
static constexpr const char *k_wasi_arch = "wasm32";

std::string CodegenTarget::effective_triple() const
{
    return triple.empty() ? llvm::sys::getDefaultTargetTriple() : triple;
}

bool CodegenTarget::is_windows() const
{
    return llvm::Triple(effective_triple()).isOSWindows();
}

bool CodegenTarget::is_darwin() const
{
    return llvm::Triple(effective_triple()).isOSDarwin();
}

std::optional<const char *> CodegenTarget::entry_symbol() const
{
    if (exec_model == ExecModel::t_library) {
        return std::nullopt;
    }

    return is_wasm() ? "__main_argc_argv" : ECO_ENTRY_SYMBOL_NAME;
}

std::span<const char *const> CodegenTarget::reserved_entry_names() const
{
    static const char *const k_main[] = { ECO_ENTRY_SYMBOL_NAME };
    static const char *const k_wasi_command[] = { ECO_ENTRY_SYMBOL_NAME, "__main_argc_argv" };

    if (is_wasm() && exec_model != ExecModel::t_library) {
        return k_wasi_command;
    }

    return k_main;
}

unsigned CodegenTarget::entry_arg_count() const
{
    return is_wasm() ? 2u : 3u;
}

bool CodegenTarget::uses_pic() const
{
    return !is_wasm();
}

bool CodegenTarget::snapshots_bitcode() const
{
    return !is_wasm();
}

const char *CodegenTarget::output_extension() const
{
    if (is_wasm()) {
        return ".wasm";
    }

    if (exec_model != ExecModel::t_library) {
        return "";
    }

    // native lib is never a real cross (iOS is refused, wasi is `.wasm` above), so
    // the suffix is this host's, the same rule `output_path` uses for `.exe`
#if defined(__APPLE__)
    return ".dylib";
#elif defined(_WIN32)
    return ".dll";
#else
    return ".so";
#endif
}

std::filesystem::path CodegenTarget::output_path(std::filesystem::path path) const
{
    if (!path.extension().empty()) {
        return path;
    }

    const char *ext = output_extension();
    if (ext[0] != '\0') {
        path += ext;
        return path;
    }

#if defined(_WIN32)
    path += ".exe";
#endif
    return path;
}

bool CodegenTarget::has_os_threads() const
{
    return !is_wasm();
}

std::string facts_architecture(
    bool ios_device,
    const std::string &arch_override,
    const std::string &target_os,
    bool emitting)
{
    if (ios_device && arch_override.empty()) {
        return k_ios_device_arch;
    }

    if (emitting && target_os == "wasi" && arch_override.empty()) {
        return k_wasi_arch;
    }

    return arch_override;
}

bool resolve_codegen_target(
    const TargetFacts &facts,
    bool ios_device,
    const std::string &arch_override,
    CodegenTarget &out_target,
    std::string &out_error)
{
    out_target = {};

    const bool host_is_darwin = TargetFacts::host().operating_system == "darwin";

    if (ios_device) {
        if (facts.operating_system != "ios") {
            out_error = "--ios-device needs --target-os ios";
            return false;
        }

        if (!host_is_darwin) {
            out_error = "--ios-device cross-compiles for a physical iPhone, which needs a Darwin host";
            return false;
        }

        // device is arm64; an explicit `--target-arch x86_64` would emit a phone
        // that does not exist. host x86_64 without the flag still becomes arm64
        // below - the phone's arch, not this machine's. facts_architecture is
        // the matching half, so a condition sees arm64 too
        if (!arch_override.empty() && arch_override != k_ios_device_arch) {
            out_error = fmt::format(
                "--ios-device is arm64; --target-arch '{}' is not a phone", arch_override);
            return false;
        }
    }

    if (facts.operating_system == "wasi") {
        // wasi is wasm32 on every host. an explicit `--target-arch arm64` would
        // emit a module that is not wasm while conditions still said wasi
        if (!arch_override.empty() && arch_override != k_wasi_arch) {
            out_error = fmt::format(
                "wasi is wasm32; --target-arch '{}' is not", arch_override);
            return false;
        }

        out_target.triple = k_wasi_triple;
        out_target.wasm = true;
        out_target.pointer_bytes = 4;
        return true;
    }

    if (facts.operating_system != "ios" || !host_is_darwin) {
        return true;
    }

    if (ios_device) {
        out_target.triple = std::string("arm64-apple-ios") + k_ios_min_version;
        out_target.apple_sdk = "iphoneos";
        return true;
    }

    const std::string arch = facts.architecture.empty() ? "arm64" : facts.architecture;
    out_target.triple = arch + "-apple-ios" + k_ios_min_version + "-simulator";
    out_target.apple_sdk = "iphonesimulator";

    return true;
}

};
