#include "Compiler/CodegenTarget.h"

#include "Compiler/RuntimeSymbols.h"
#include "Compiler/TargetFacts.h"

#include "eco.h"

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

#include <filesystem>
#include <optional>
#include <span>
#include <utility>

namespace Compiler
{

static constexpr const char *k_ios_min_version = "15.0";
static constexpr const char *k_ios_device_arch = "arm64";
static constexpr const char *k_wasi_arch = "wasm32";
static constexpr const char *k_wasi_os = "wasi";

namespace
{
    // the os column, from a triple. asked when a row is built.
    // WASI uses isOSWASI, the same predicate TargetFacts::operating_system_of uses;
    // wasm ISA is a separate column (_wasm_arch)
    TargetOs os_of(const llvm::Triple &triple)
    {
        if (triple.isOSWASI()) {
            return TargetOs::t_wasi;
        }

        if (triple.isiOS()) {
            return TargetOs::t_ios;
        }

        if (triple.isOSDarwin()) {
            return TargetOs::t_darwin;
        }

        if (triple.isOSWindows()) {
            return TargetOs::t_windows;
        }

        if (triple.isOSLinux()) {
            return TargetOs::t_linux;
        }

        return TargetOs::t_other;
    }

    const char *const k_entry_main[] = { ECO_ENTRY_SYMBOL_NAME };
    const char *const k_entry_wasi[] = { ECO_ENTRY_SYMBOL_NAME, "__main_argc_argv" };
}

CodegenTarget::CodegenTarget()
    : CodegenTarget(std::string(), std::string())
{
}

CodegenTarget::CodegenTarget(std::string triple, std::string apple_sdk)
    : _triple(std::move(triple))
    , _apple_sdk(std::move(apple_sdk))
{
    const llvm::Triple parsed(effective_triple());
    _os = os_of(parsed);
    _wasm_arch = parsed.isWasm();
    _pointer_bytes = parsed.isArch32Bit() ? 4 : 8;
}

CodegenTarget CodegenTarget::wasi()
{
    return for_triple(k_wasi_triple);
}

CodegenTarget CodegenTarget::for_triple(std::string triple)
{
    return CodegenTarget(std::move(triple), std::string());
}

CodegenTarget CodegenTarget::ios_simulator(const std::string &arch)
{
    return CodegenTarget(arch + "-apple-ios" + k_ios_min_version + "-simulator", "iphonesimulator");
}

CodegenTarget CodegenTarget::ios_device()
{
    return CodegenTarget(std::string(k_ios_device_arch) + "-apple-ios" + k_ios_min_version, "iphoneos");
}

CodegenTarget CodegenTarget::for_program(ExecModel model) const
{
    CodegenTarget row = *this;
    row._exec_model = model;
    return row;
}

std::string CodegenTarget::effective_triple() const
{
    return _triple.empty() ? llvm::sys::getDefaultTargetTriple() : _triple;
}

std::optional<const char *> CodegenTarget::entry_symbol() const
{
    if (is_library()) {
        return std::nullopt;
    }

    return is_wasi() ? k_entry_wasi[1] : ECO_ENTRY_SYMBOL_NAME;
}

ReservedExportNames CodegenTarget::reserved_export_names() const
{
    ReservedExportNames out;

    // wasi command: `__main_argc_argv`. everything else: `main`
    out.entry = is_wasi() && !is_library()
        ? std::span<const char *const>(k_entry_wasi)
        : std::span<const char *const>(k_entry_main);

    out.runtime = reserved_c_export_names();
    out.runtime_prefix = k_compiler_runtime_prefix;

    if (is_wasi()) {
        out.wasm_crt = wasm_crt_names();
    }

    return out;
}

unsigned CodegenTarget::entry_arg_count() const
{
    return is_wasi() ? 2u : 3u;
}

EnvironSource CodegenTarget::environ_source() const
{
    switch (_os) {
        case TargetOs::t_wasi: return EnvironSource::t_wasilibc_get_environ;
        case TargetOs::t_darwin: return EnvironSource::t_ns_get_environ;
        case TargetOs::t_ios: return EnvironSource::t_ns_get_environ;
        case TargetOs::t_windows: return EnvironSource::t_none;
        case TargetOs::t_linux: return EnvironSource::t_libc_environ;
        case TargetOs::t_other: return EnvironSource::t_libc_environ;
    }

    assert(false && "unhandled TargetOs in environ_source");
    return EnvironSource::t_libc_environ;
}

StdioUnbuffering CodegenTarget::stdio_unbuffering() const
{
    switch (_os) {
        case TargetOs::t_windows: return StdioUnbuffering::t_ucrt_stdout_stderr;
        case TargetOs::t_wasi: return StdioUnbuffering::t_wasi_libc_stdout;
        case TargetOs::t_darwin: return StdioUnbuffering::t_none;
        case TargetOs::t_ios: return StdioUnbuffering::t_none;
        case TargetOs::t_linux: return StdioUnbuffering::t_none;
        case TargetOs::t_other: return StdioUnbuffering::t_none;
    }

    assert(false && "unhandled TargetOs in stdio_unbuffering");
    return StdioUnbuffering::t_none;
}

EntropySource CodegenTarget::entropy_source() const
{
    switch (_os) {
        case TargetOs::t_darwin: return EntropySource::t_arc4random;
        case TargetOs::t_ios: return EntropySource::t_arc4random;
        case TargetOs::t_windows: return EntropySource::t_rand_s;
        case TargetOs::t_linux: return EntropySource::t_getentropy;
        case TargetOs::t_wasi: return EntropySource::t_getentropy;
        case TargetOs::t_other: return EntropySource::t_getentropy;
    }

    assert(false && "unhandled TargetOs in entropy_source");
    return EntropySource::t_getentropy;
}

HostSymbolLookup CodegenTarget::host_symbol_lookup() const
{
    switch (_os) {
        case TargetOs::t_windows: return HostSymbolLookup::t_get_proc_address;
        case TargetOs::t_darwin: return HostSymbolLookup::t_dlsym;
        case TargetOs::t_ios: return HostSymbolLookup::t_dlsym;
        case TargetOs::t_linux: return HostSymbolLookup::t_dlsym;
        case TargetOs::t_wasi: return HostSymbolLookup::t_dlsym;
        case TargetOs::t_other: return HostSymbolLookup::t_dlsym;
    }

    assert(false && "unhandled TargetOs in host_symbol_lookup");
    return HostSymbolLookup::t_dlsym;
}

int64_t CodegenTarget::rtld_default() const
{
    switch (_os) {
        case TargetOs::t_darwin: return -2;
        case TargetOs::t_ios: return -2;
        case TargetOs::t_linux: return 0;
        case TargetOs::t_wasi: return 0;
        case TargetOs::t_windows: return 0;
        case TargetOs::t_other: return 0;
    }

    assert(false && "unhandled TargetOs in rtld_default");
    return 0;
}

bool CodegenTarget::uses_pic() const
{
    return !is_wasm();
}

bool CodegenTarget::snapshots_bitcode() const
{
    // wasm ISA stays in this LLVMContext. a Windows-hosted round-trip failure is
    // Emit.cpp's: a fact about this process, not this row
    return !is_wasm();
}

const char *CodegenTarget::library_suffix() const
{
    // the ISA, not the OS column. wasm32-unknown-unknown is t_other and would
    // otherwise be named like an ELF shared object
    if (is_wasm()) {
        return ".wasm";
    }

    switch (_os) {
        case TargetOs::t_wasi: return ".wasm";
        case TargetOs::t_darwin: return ".dylib";
        case TargetOs::t_ios: return ".dylib";
        case TargetOs::t_windows: return ".dll";
        case TargetOs::t_linux: return ".so";
        case TargetOs::t_other: return ".so";
    }

    assert(false && "unhandled TargetOs in library_suffix");
    return ".so";
}

const char *CodegenTarget::output_extension() const
{
    if (is_wasm()) {
        return ".wasm";
    }

    if (is_library()) {
        return library_suffix();
    }

    // a Windows *command*, not "this process is Windows". a Linux-hosted
    // `for_triple("x86_64-pc-windows-msvc")` still names an .exe, and a
    // Windows-hosted Linux row does not
    if (is_windows()) {
        return ".exe";
    }

    return "";
}

std::string CodegenTarget::library_file_name(const std::string &stem) const
{
    const char *ext = library_suffix();

    // Windows and wasm have no `lib` prefix. Darwin and ELF do, so another
    // module can `-l` the image by the stem the target named
    if (is_wasm() || is_windows()) {
        return stem + ext;
    }

    return std::string("lib") + stem + ext;
}

std::optional<const char *> library_refusal(const TargetFacts &facts)
{
    // a phone does not dlopen an app-side dylib, host row included
    if (facts.operating_system == "ios") {
        return "a native 'lib' target is a loadable library, and iOS does not load "
            "one from an app";
    }

    return std::nullopt;
}

std::filesystem::path CodegenTarget::output_path(std::filesystem::path path) const
{
    if (!path.extension().empty()) {
        return path;
    }

    const char *ext = output_extension();
    if (ext[0] != '\0') {
        path += ext;
    }

    return path;
}

bool resolve_codegen_target(
    const TargetFacts &facts,
    bool ios_device,
    CodegenTarget &out_target,
    std::string &out_error)
{
    out_target = CodegenTarget();

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
        // that does not exist. TargetFacts::resolve already filled arm64 when
        // the flag was omitted, so a condition sees the phone's arch too
        if (!facts.architecture.empty() && facts.architecture != k_ios_device_arch) {
            out_error = fmt::format(
                "--ios-device is arm64; --target-arch '{}' is not a phone", facts.architecture);
            return false;
        }
    }

    if (facts.operating_system == k_wasi_os) {
        out_target = CodegenTarget::wasi();
        return true;
    }

    if (facts.operating_system != "ios" || !host_is_darwin) {
        return true;
    }

    if (ios_device) {
        out_target = CodegenTarget::ios_device();
        return true;
    }

    out_target = CodegenTarget::ios_simulator(facts.architecture.empty() ? "arm64" : facts.architecture);
    return true;
}

};
