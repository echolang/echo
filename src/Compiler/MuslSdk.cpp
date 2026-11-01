#include "Compiler/HostTool.h"

#include "Compiler/CodegenTarget.h"

#include <fmt/core.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr const char *k_musl_sysroot_need =
        "lib/libc.a, include/, musl crt objects, and libclang_rt.builtins.a";

    bool looks_like_musl_sysroot(const std::filesystem::path &root)
    {
        std::error_code ec;
        const std::filesystem::path lib = root / "lib";
        return std::filesystem::is_regular_file(lib / "libc.a", ec)
            && std::filesystem::is_directory(root / "include", ec)
            && std::filesystem::is_regular_file(lib / "crt1.o", ec)
            && std::filesystem::is_regular_file(lib / "crti.o", ec)
            && std::filesystem::is_regular_file(lib / "crtn.o", ec)
            && std::filesystem::is_regular_file(lib / "libclang_rt.builtins.a", ec);
    }

    struct MuslSysroot
    {
        std::filesystem::path path;
        std::string error;
    };

    MuslSysroot resolve_musl_sysroot()
    {
        if (const char *env = std::getenv("MUSL_SYSROOT")) {
            const std::filesystem::path asked(env);
            if (!asked.empty()) {
                if (looks_like_musl_sysroot(asked)) {
                    return { asked, {} };
                }

                return {
                    {},
                    fmt::format("MUSL_SYSROOT '{}' has no {}", asked.string(), k_musl_sysroot_need)
                };
            }
        }

        const std::filesystem::path dir = Compiler::process_directory();
        const std::filesystem::path candidates[] = {
            dir / "sysroot",
            dir.parent_path() / "sysroot",
            dir.parent_path() / "lib" / "echo" / "sysroot",
        };

        for (const std::filesystem::path &candidate : candidates) {
            if (looks_like_musl_sysroot(candidate)) {
                return { candidate, {} };
            }
        }

        return {
            {},
            fmt::format(
                "musl sysroot not found. Set MUSL_SYSROOT to a prefix with {}, or install a "
                "released Linux echoc that ships one",
                k_musl_sysroot_need)
        };
    }
};

std::filesystem::path Compiler::musl_sysroot()
{
    return resolve_musl_sysroot().path;
}

bool Compiler::append_musl_target_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    std::string &out_error
)
{
    const MuslSysroot root = resolve_musl_sysroot();
    if (root.path.empty()) {
        out_error = root.error;
        return false;
    }

    argv.push_back("-target");
    argv.push_back(target.effective_triple());
    argv.push_back("--sysroot");
    argv.push_back(root.path.string());
    argv.push_back("-static");
    return true;
}

void Compiler::append_musl_link_tail(
    std::vector<std::string> &argv,
    const std::vector<std::string> &link_words
)
{
    const std::filesystem::path sysroot = musl_sysroot();
    if (sysroot.empty()) {
        return;
    }

    const std::filesystem::path lib = sysroot / "lib";
    argv.push_back("-Wl,--start-group");
    argv.insert(argv.end(), link_words.begin(), link_words.end());
    argv.push_back("-lc");
    argv.push_back((lib / "libclang_rt.builtins.a").string());
    argv.push_back("-Wl,--end-group");
    argv.push_back((lib / "crtn.o").string());
}
