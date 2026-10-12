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

        std::filesystem::path best;
        for (const auto &entry : std::filesystem::directory_iterator(lib_clang, ec)) {
            if (!entry.is_directory(ec) || !looks_like_wasi_resource_dir(entry.path())) {
                continue;
            }

            if (best.empty() || entry.path().filename() > best.filename()) {
                best = entry.path();
            }
        }

        return best;
    }

    const std::filesystem::path k_sdk_prefixes[] = {
        "/opt/wasi-sdk",
        "/opt/homebrew/opt/wasi-sdk",
        "/usr/local/opt/wasi-sdk",
    };

    Compiler::WasiSdk sdk_from_prefix(const std::filesystem::path &prefix)
    {
        Compiler::WasiSdk out;
        out.sysroot = wasi_sysroot_at(prefix);
        out.resource_dir = resource_dir_in_sdk(prefix);
        out.clang = clang_in_sdk(prefix);
        return out;
    }

    bool is_complete_sdk(const Compiler::WasiSdk &sdk)
    {
        return !sdk.sysroot.empty() && !sdk.clang.empty();
    }

    void fill_split_install(Compiler::WasiSdk &out)
    {
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
    }

    void seat_wasm_ld(Compiler::WasiSdk &out, bool closed)
    {
        if (!out.clang.empty()) {
            out.wasm_ld = tool_in_sdk(out.clang.parent_path().parent_path(), "wasm-ld");
            return;
        }

        if (closed) {
            return;
        }

        auto found = llvm::sys::findProgramByName("wasm-ld");
        if (found) {
            out.wasm_ld = found.get();
        }
    }

    Compiler::WasiSdk resolve_wasi_sdk()
    {
        Compiler::WasiSdk out;
        bool closed = false;

        if (const char *asked = std::getenv("WASI_SDK_PATH"); asked && *asked) {
            out = sdk_from_prefix(asked);
            closed = true;
        }
        else {
            std::vector<std::filesystem::path> prefixes(
                std::begin(k_sdk_prefixes), std::end(k_sdk_prefixes));

#if defined(_WIN32)
            const std::filesystem::path dir = Compiler::process_directory();
            prefixes.insert(prefixes.begin(), dir.parent_path() / "wasi-sdk");
            prefixes.insert(prefixes.begin(), dir / "wasi-sdk");
#endif

            for (const std::filesystem::path &prefix : prefixes) {
                Compiler::WasiSdk candidate = sdk_from_prefix(prefix);
                if (is_complete_sdk(candidate)) {
                    out = candidate;
                    break;
                }
            }
        }

        if (!closed) {
            fill_split_install(out);
        }

        seat_wasm_ld(out, closed);
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
        out_error = "WASI SDK not found. Set WASI_SDK_PATH to a complete SDK prefix "
            "(clang and sysroot together)"
#if defined(_WIN32)
            ", or place a wasi-sdk next to echoc"
#else
            ", or install /opt/wasi-sdk or Homebrew wasi-sdk"
#endif
            ;
        return false;
    }

    // a scavenged wasi-libc sysroot plus PATH clang is how a host libc object
    // used to be linked into a wasm module. cc_driver refuses that mix by
    // returning no program; this is the sentence, before anything is spawned
    if (sdk.clang.empty()) {
        out_error = "WASI sysroot has no clang in the same prefix. Set WASI_SDK_PATH "
            "to a complete SDK (clang and sysroot together), not a wasi-libc checkout";
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
