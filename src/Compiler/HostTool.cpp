#include "Compiler/HostTool.h"

#include "Compiler/CodegenTarget.h"

#include <llvm/Support/FileSystem.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

std::string Compiler::cc_driver(const CodegenTarget &target)
{
    if (target.is_wasi()) {
        const std::filesystem::path clang = wasi_sdk().clang;
        if (!clang.empty()) {
            return clang.string();
        }

        // sysroot without a clang in the same prefix: do not mix PATH clang
        // with a scavenged wasi-libc
        if (!wasi_sdk().sysroot.empty()) {
            return {};
        }
    }

    return "clang";
}

std::filesystem::path Compiler::process_executable()
{
    static const std::filesystem::path exe = [] {
#if defined(_WIN32)
        std::wstring buf(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0 || n >= buf.size()) {
            return std::filesystem::path();
        }
        buf.resize(n);
        return std::filesystem::path(buf);
#else
        return std::filesystem::path(llvm::sys::fs::getMainExecutable(
            "echoc", reinterpret_cast<void *>(&Compiler::process_executable)));
#endif
    }();

    return exe;
}

std::filesystem::path Compiler::process_directory()
{
    const std::filesystem::path &exe = process_executable();

    if (exe.empty()) {
        return std::filesystem::current_path();
    }

    return exe.parent_path();
}

std::filesystem::path Compiler::windows_sysroot()
{
#if !defined(_WIN32)
    return {};
#else
    static const std::filesystem::path root = [] {
        std::error_code ec;
        const std::filesystem::path dir = Compiler::process_directory();
        const std::filesystem::path candidates[] = {
            dir / "sysroot",
            dir.parent_path() / "sysroot",
        };

        for (const std::filesystem::path &candidate : candidates) {
            if (std::filesystem::is_directory(candidate / "lib", ec)) {
                return candidate;
            }
        }

        return std::filesystem::path();
    }();

    return root;
#endif
}

namespace
{
    std::string trim_right(const std::string &text)
    {
        const size_t end = text.find_last_not_of(" \t\n\r");
        if (end == std::string::npos) {
            return {};
        }

        return text.substr(0, end + 1);
    }

    std::filesystem::path resource_include_beside(const std::filesystem::path &clang)
    {
        std::error_code ec;
        const std::filesystem::path libclang = clang.parent_path().parent_path() / "lib" / "clang";
        if (!std::filesystem::is_directory(libclang, ec)) {
            return {};
        }

        std::filesystem::path found;
        for (const auto &entry : std::filesystem::directory_iterator(libclang, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }

            const std::filesystem::path include = entry.path() / "include";
            if (std::filesystem::is_directory(include, ec)) {
                found = include;
            }
        }

        return found;
    }

    // the include directory of the clang `#[cc:]` actually invokes. cached:
    // `clang -print-resource-dir` is one spawn per process, and a miss falls
    // through to the `lib/clang/<ver>/include` next to that binary
    std::filesystem::path clang_resource_include()
    {
        static const std::filesystem::path dir = [] {
            const Compiler::CapturedProcess printed =
                Compiler::run_captured({ Compiler::host_clang(), "-print-resource-dir" });

            if (printed.exit_code == 0) {
                const std::string root = trim_right(printed.output);
                if (!root.empty()) {
                    std::error_code ec;
                    const std::filesystem::path include = std::filesystem::path(root) / "include";
                    if (std::filesystem::is_directory(include, ec)) {
                        return include;
                    }
                }
            }

            return resource_include_beside(Compiler::host_clang());
        }();

        return dir;
    }
};

void Compiler::append_windows_sysroot_cc_args(std::vector<std::string> &argv)
{
#if !defined(_WIN32)
    (void)argv;
#else
    // Backend::host_linker_command always `/defaultlib:libcmt`. clang's
    // default on Windows is the DLL CRT, which is how a `#[cc:]` object
    // (or a static libcurl built /MD) pulled ucrt.lib in next to libucrt
    // and duplicate-symbol'd malloc. asked even when there is no sysroot,
    // because the CRT choice is the linker's, not the bundle's
    argv.push_back("-fms-runtime-lib=static");
    append_windows_sysroot_cc_args(argv, windows_sysroot());
#endif
}

void Compiler::append_windows_sysroot_cc_args(std::vector<std::string> &argv, const std::filesystem::path &sysroot)
{
    if (sysroot.empty()) {
        return;
    }

    const std::filesystem::path include = sysroot / "include";
    std::error_code ec;
    if (!std::filesystem::is_directory(include, ec)) {
        return;
    }

    std::vector<std::filesystem::path> dirs = { include };
    for (const auto &entry : std::filesystem::directory_iterator(include, ec)) {
        if (entry.is_directory(ec)) {
            dirs.push_back(entry.path());
        }
    }

    // **clang's builtins first.** Command-line `-isystem` is searched ahead of
    // the resource directory, so a bundled MSVC emmintrin.h would win and
    // `_mm_*` would become linker symbols GNU-driver clang does not
    // implement. putting the resource include on the command line first keeps
    // the builtins ahead while the sysroot stays `-isystem`, so windows.h
    // still beats a host VS. `-isystem-after` would also save the builtins,
    // and would let that host VS beat the bundle
    const std::filesystem::path builtins = clang_resource_include();
    if (!builtins.empty()) {
        argv.push_back("-isystem");
        argv.push_back(builtins.string());
    }

    std::sort(dirs.begin(), dirs.end());
    for (const std::filesystem::path &dir : dirs) {
        argv.push_back("-isystem");
        argv.push_back(dir.string());
    }
}

void Compiler::append_windows_sysroot_link_args(std::vector<std::string> &argv)
{
#if !defined(_WIN32)
    (void)argv;
#else
    argv.push_back("-fuse-ld=lld");
    argv.push_back("-fms-runtime-lib=static");
    const std::filesystem::path sysroot = windows_sysroot();
    if (sysroot.empty()) {
        return;
    }
    argv.push_back("-L" + (sysroot / "lib").string());
#endif
}

bool Compiler::append_codegen_target_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    std::string &out_error
)
{
    if (target.is_wasi()) {
        return append_wasi_target_args(argv, target, out_error);
    }

    if (target.is_darwin()) {
        return append_apple_target_args(argv, target, out_error);
    }

    if (target.is_windows()) {
        append_windows_sysroot_cc_args(argv);
        return true;
    }

    return true;
}

bool Compiler::append_codegen_link_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    bool export_memory,
    std::string &out_error,
    const std::string &output_path
)
{
    // compile -isystem directories do not belong on a link line, and the Windows
    // link helper already carries the CRT flag those compile args also set.
    // calling both doubled -fms-runtime-lib=static and pasted the include set
    // onto the link
    if (target.is_windows()) {
        append_windows_sysroot_link_args(argv);
    }
    else if (!append_codegen_target_args(argv, target, out_error)) {
        return false;
    }

    // clang turns this into `wasm-ld ... crt1-reactor.o --entry _initialize`. a C
    // compile must not see it: CBuild reuses append_codegen_target_args
    if (target.is_wasi() && target.is_library()) {
        argv.push_back("-mexec-model=reactor");
    }

    // native library: clang -shared, the same driver CBuild trusts for a loadable
    // object. wasm stays the reactor flag above; the Darwin ld / lld-link fast
    // path is skipped in Backend::link_program for this kind
    if (target.is_native_library()) {
        argv.push_back("-shared");

        if (!output_path.empty()) {
            const std::string basename = std::filesystem::path(output_path).filename().string();

            if (target.is_darwin()) {
                argv.push_back("-Wl,-install_name,@rpath/" + basename);
            }
            else if (target.is_windows()) {
                // MSVC link refuses a .dll as input (LNK1107). the import
                // library sits next to the image so a C host can name it
                std::filesystem::path implib = output_path;
                implib.replace_extension(".lib");
                argv.push_back("-Wl,/IMPLIB:" + implib.string());
            }
            else {
                argv.push_back("-Wl,-soname," + basename);
                argv.push_back("-Wl,-z,defs");
                // the lib's own calls to an export stay inside the image.
                // without it, a plugin that exports `floor` binds to libm
                argv.push_back("-Wl,-Bsymbolic-functions");
                // gen_startup copies the host's hash seed through dlsym
                argv.push_back("-ldl");
            }
        }
    }

    // a plugin dlsyms this out of the main image. ELF executables keep defined
    // symbols local unless named; Darwin exports globals by default
    if (!target.is_library() && target.os() == TargetOs::t_linux) {
        argv.push_back("-Wl,--export-dynamic-symbol=__eco_hash_seed");
    }

    if (target.is_wasm() && export_memory) {
        argv.push_back("-Wl,--export-memory");
    }

    return true;
}
