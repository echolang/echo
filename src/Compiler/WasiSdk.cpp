#include "Compiler/HostTool.h"

#include "Compiler/CodegenTarget.h"

#include <llvm/Support/Program.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    bool looks_like_sysroot(const std::filesystem::path &root)
    {
        std::error_code ec;
        return std::filesystem::is_directory(root / "include", ec);
    }

    std::filesystem::path wasi_sysroot_at(const std::filesystem::path &prefix)
    {
        if (prefix.empty()) {
            return {};
        }

        const std::filesystem::path nested = prefix / "share" / "wasi-sysroot";
        if (looks_like_sysroot(nested)) {
            return nested;
        }

        if (looks_like_sysroot(prefix)) {
            return prefix;
        }

        return {};
    }

    bool looks_like_wasi_resource_dir(const std::filesystem::path &root)
    {
        std::error_code ec;
        const std::filesystem::path candidates[] = {
            root / "lib" / Compiler::k_wasi_triple / "libclang_rt.builtins.a",
            root / "lib" / "wasi" / "libclang_rt.builtins-wasm32.a",
            root / "lib" / "wasi" / "libclang_rt.builtins.a",
        };

        for (const std::filesystem::path &candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return true;
            }
        }

        return false;
    }

    std::filesystem::path tool_in_sdk(const std::filesystem::path &sdk, const char *name)
    {
        if (sdk.empty()) {
            return {};
        }

        std::filesystem::path tool = sdk / "bin" / name;
#if defined(_WIN32)
        tool += ".exe";
#endif
        std::error_code ec;
        if (std::filesystem::is_regular_file(tool, ec)) {
            return tool;
        }

        return {};
    }

    std::filesystem::path clang_in_sdk(const std::filesystem::path &sdk)
    {
        return tool_in_sdk(sdk, "clang");
    }

    std::filesystem::path resource_dir_in_sdk(const std::filesystem::path &sdk)
    {
        if (sdk.empty()) {
            return {};
        }

        const std::filesystem::path lib_clang = sdk / "lib" / "clang";
        std::error_code ec;
        if (!std::filesystem::is_directory(lib_clang, ec)) {
            return {};
        }

        for (const auto &entry : std::filesystem::directory_iterator(lib_clang, ec)) {
            if (entry.is_directory(ec) && looks_like_wasi_resource_dir(entry.path())) {
                return entry.path();
            }
        }

        return {};
    }

    const std::filesystem::path k_sdk_prefixes[] = {
        "/opt/wasi-sdk",
        "/opt/homebrew/opt/wasi-sdk",
        "/usr/local/opt/wasi-sdk",
    };

    void fill_from_prefix(Compiler::WasiSdk &out, const std::filesystem::path &prefix)
    {
        if (out.sysroot.empty()) {
            out.sysroot = wasi_sysroot_at(prefix);
        }
        if (out.resource_dir.empty()) {
            out.resource_dir = resource_dir_in_sdk(prefix);
        }
        if (out.clang.empty()) {
            out.clang = clang_in_sdk(prefix);
        }
    }

    Compiler::WasiSdk resolve_wasi_sdk()
    {
        Compiler::WasiSdk out;

        if (const char *asked = std::getenv("WASI_SDK_PATH")) {
            fill_from_prefix(out, asked);
        }

        for (const std::filesystem::path &prefix : k_sdk_prefixes) {
            fill_from_prefix(out, prefix);
        }

        if (out.sysroot.empty()) {
            const std::filesystem::path libc_prefixes[] = {
                "/opt/homebrew/opt/wasi-libc",
                "/usr/local/opt/wasi-libc",
            };

            for (const std::filesystem::path &prefix : libc_prefixes) {
                out.sysroot = wasi_sysroot_at(prefix);
                if (!out.sysroot.empty()) {
                    break;
                }
            }
        }

        if (out.resource_dir.empty()) {
            const std::filesystem::path runtimes[] = {
                "/opt/homebrew/opt/wasi-runtimes/share/wasi-runtimes",
                "/usr/local/opt/wasi-runtimes/share/wasi-runtimes",
            };

            for (const std::filesystem::path &candidate : runtimes) {
                if (looks_like_wasi_resource_dir(candidate)) {
                    out.resource_dir = candidate;
                    break;
                }
            }
        }

        // wasm-ld belongs next to the clang this row already chose, so a
        // later prefix cannot mix a linker from a different SDK
        if (!out.clang.empty()) {
            std::filesystem::path beside = out.clang.parent_path() / "wasm-ld";
#if defined(_WIN32)
            beside += ".exe";
#endif
            std::error_code ec;
            if (std::filesystem::is_regular_file(beside, ec)) {
                out.wasm_ld = beside;
            }
        }

        if (out.wasm_ld.empty()) {
            auto found = llvm::sys::findProgramByName("wasm-ld");
            if (found) {
                out.wasm_ld = found.get();
            }
        }

        return out;
    }
};

const Compiler::WasiSdk &Compiler::wasi_sdk()
{
    static const WasiSdk sdk = resolve_wasi_sdk();
    return sdk;
}

std::filesystem::path Compiler::wasi_sysroot()
{
    return wasi_sdk().sysroot;
}

std::filesystem::path Compiler::wasi_resource_dir()
{
    return wasi_sdk().resource_dir;
}

std::filesystem::path Compiler::wasi_clang()
{
    return wasi_sdk().clang;
}

std::filesystem::path Compiler::wasi_wasm_ld()
{
    return wasi_sdk().wasm_ld;
}

bool Compiler::append_wasi_target_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    std::string &out_error
)
{
    const WasiSdk &sdk = wasi_sdk();
    if (sdk.sysroot.empty()) {
        out_error = "WASI SDK not found. Set WASI_SDK_PATH, or install a WASI SDK "
            "(/opt/wasi-sdk) or Homebrew wasi-libc";
        return false;
    }

    if (sdk.wasm_ld.empty()) {
        out_error = "no wasm-ld next to the clang driving this WASI build. "
            "Install a WASI SDK and set WASI_SDK_PATH so clang and wasm-ld sit in the same bin/";
        return false;
    }

    argv.push_back("-target");
    argv.push_back(target.effective_triple());
    argv.push_back("--sysroot");
    argv.push_back(sdk.sysroot.string());
    argv.push_back("-fuse-ld=" + sdk.wasm_ld.string());

    if (!sdk.resource_dir.empty()) {
        argv.push_back("-resource-dir");
        argv.push_back(sdk.resource_dir.string());
    }
    else {
        argv.push_back("-rtlib=none");
    }

    return true;
}
