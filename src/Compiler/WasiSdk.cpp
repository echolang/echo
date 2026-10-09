#include "Compiler/HostTool.h"

#include "Compiler/CodegenTarget.h"

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
        return std::filesystem::is_regular_file(
            root / "lib" / Compiler::k_wasi_triple / "libclang_rt.builtins.a", ec);
    }
};

std::filesystem::path Compiler::wasi_sysroot()
{
    static const std::filesystem::path root = [] {
        if (const char *asked = std::getenv("WASI_SDK_PATH")) {
            const std::filesystem::path from_env = wasi_sysroot_at(asked);
            if (!from_env.empty()) {
                return from_env;
            }
        }

        const std::filesystem::path candidates[] = {
            "/opt/wasi-sdk",
            "/opt/homebrew/opt/wasi-sdk",
            "/usr/local/opt/wasi-sdk",
            "/opt/homebrew/opt/wasi-libc",
            "/usr/local/opt/wasi-libc",
        };

        for (const std::filesystem::path &candidate : candidates) {
            const std::filesystem::path found = wasi_sysroot_at(candidate);
            if (!found.empty()) {
                return found;
            }
        }

        return std::filesystem::path();
    }();

    return root;
}

std::filesystem::path Compiler::wasi_resource_dir()
{
    static const std::filesystem::path root = [] {
        if (const char *asked = std::getenv("WASI_SDK_PATH")) {
            const std::filesystem::path sdk(asked);
            const std::filesystem::path lib_clang = sdk / "lib" / "clang";
            std::error_code ec;
            if (std::filesystem::is_directory(lib_clang, ec)) {
                for (const auto &entry : std::filesystem::directory_iterator(lib_clang, ec)) {
                    if (entry.is_directory(ec) && looks_like_wasi_resource_dir(entry.path())) {
                        return entry.path();
                    }
                }
            }
        }

        const std::filesystem::path candidates[] = {
            "/opt/homebrew/opt/wasi-runtimes/share/wasi-runtimes",
            "/usr/local/opt/wasi-runtimes/share/wasi-runtimes",
        };

        for (const std::filesystem::path &candidate : candidates) {
            if (looks_like_wasi_resource_dir(candidate)) {
                return candidate;
            }
        }

        return std::filesystem::path();
    }();

    return root;
}

bool Compiler::append_wasi_target_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    std::string &out_error
)
{
    const std::filesystem::path sysroot = wasi_sysroot();
    if (sysroot.empty()) {
        out_error = "WASI SDK not found. Set WASI_SDK_PATH, or install a WASI SDK "
            "(/opt/wasi-sdk) or Homebrew wasi-libc";
        return false;
    }

    argv.push_back("-target");
    argv.push_back(target.effective_triple());
    argv.push_back("--sysroot");
    argv.push_back(sysroot.string());
    argv.push_back("-fuse-ld=lld");

    const std::filesystem::path resource = wasi_resource_dir();
    if (!resource.empty()) {
        argv.push_back("-resource-dir");
        argv.push_back(resource.string());
    }
    else {
        argv.push_back("-rtlib=none");
    }

    return true;
}
