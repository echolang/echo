#include <catch2/catch_test_macros.hpp>

#include <Compiler/CodegenTarget.h>
#include <Compiler/HostTool.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "subprocess.h"

#if !defined(_WIN32)
namespace
{
    struct EnvRestore
    {
        const char *name;
        std::string old;
        bool had;

        EnvRestore(const char *name, const char *value)
            : name(name)
        {
            const char *prev = std::getenv(name);
            had = prev != nullptr;
            if (had) {
                old = prev;
            }
            REQUIRE(setenv(name, value, 1) == 0);
        }

        ~EnvRestore()
        {
            if (had) {
                setenv(name, old.c_str(), 1);
            } else {
                unsetenv(name);
            }
        }
    };

    void touch_empty(const std::filesystem::path &path)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        REQUIRE(out.good());
    }

    void plant_musl_sysroot(const std::filesystem::path &root)
    {
        std::error_code ec;
        std::filesystem::create_directories(root / "include", ec);
        REQUIRE(std::filesystem::is_directory(root / "include", ec));
        touch_empty(root / "lib" / "libc.a");
        touch_empty(root / "lib" / "crt1.o");
        touch_empty(root / "lib" / "crti.o");
        touch_empty(root / "lib" / "crtn.o");
        touch_empty(root / "lib" / "libclang_rt.builtins.a");
    }
};
#endif

TEST_CASE("a planted musl sysroot names the compile and link line", "[target][musl]")
{
#if defined(_WIN32)
    SKIP("MUSL_SYSROOT is a unix path");
#else
    EchoTests::ScopedProject project("musl", "planted-sysroot");
    plant_musl_sysroot(project.root());
    EnvRestore musl_sysroot_env("MUSL_SYSROOT", project.root().string().c_str());

    std::string error;
    std::vector<std::string> argv;
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-musl");

    REQUIRE(Compiler::musl_sysroot() == project.root());
    REQUIRE(Compiler::append_codegen_target_args(argv, target, error));
    REQUIRE(std::find(argv.begin(), argv.end(), "-static") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "--sysroot") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-target") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-rtlib=compiler-rt") == argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), project.root().string()) != argv.end());

    std::vector<std::string> link;
    REQUIRE(Compiler::append_codegen_link_args(link, target, false, error));
    REQUIRE(std::find(link.begin(), link.end(), "-nostdlib") != link.end());
    REQUIRE(std::any_of(link.begin(), link.end(), [](const std::string &w) {
        return w.ends_with("crt1.o");
    }));
    REQUIRE(std::any_of(link.begin(), link.end(), [](const std::string &w) {
        return w.ends_with("crti.o");
    }));
    const size_t before_tail = link.size();
    Compiler::append_musl_link_tail(link, { "-lfoo" });
    const std::vector<std::string> tail(link.begin() + static_cast<std::ptrdiff_t>(before_tail), link.end());
    REQUIRE(tail.size() == 6);
    REQUIRE(tail[0] == "-Wl,--start-group");
    REQUIRE(tail[1] == "-lfoo");
    REQUIRE(tail[2] == "-lc");
    REQUIRE(tail[3].ends_with("libclang_rt.builtins.a"));
    REQUIRE(tail[4] == "-Wl,--end-group");
    REQUIRE(tail[5].ends_with("crtn.o"));
#endif
}

TEST_CASE("append_musl_target_args names the sysroot or refuses", "[target][musl]")
{
    std::string error;
    std::vector<std::string> argv;
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-musl");

    if (Compiler::musl_sysroot().empty()) {
        REQUIRE_FALSE(Compiler::append_codegen_target_args(argv, target, error));
        REQUIRE(error.find("musl sysroot") != std::string::npos);
        return;
    }

    REQUIRE(Compiler::append_codegen_target_args(argv, target, error));
    REQUIRE(std::find(argv.begin(), argv.end(), "-static") != argv.end());
}

TEST_CASE("echoc build --target-libc musl links a static command that runs", "[target][musl]")
{
#if !defined(__linux__)
    SKIP("musl links on linux");
#else
    if (Compiler::musl_sysroot().empty()) {
        SKIP("no musl sysroot on this machine");
    }

    EchoTests::ScopedProject project("musl", "links");
    EchoTests::write_file(project.root() / "a.eco", "echo 1.5;\n");
    const EchoTests::ProcessResult built =
        project.echoc("build --target-libc musl -o a a.eco", project.root());
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const EchoTests::ProcessResult ran = EchoTests::run_binary(project.root() / "a");
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output == "1.500000\n");
#endif
}

TEST_CASE("MUSL_SYSROOT that is not a sysroot is refused", "[target][musl]")
{
#if defined(_WIN32)
    SKIP("MUSL_SYSROOT is a unix path");
#else
    EnvRestore musl_sysroot_env("MUSL_SYSROOT", "/no/such/echo-musl");

    std::string error;
    std::vector<std::string> argv;
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-musl");
    REQUIRE_FALSE(Compiler::append_codegen_target_args(argv, target, error));
    REQUIRE(error.find("MUSL_SYSROOT") != std::string::npos);
    REQUIRE(error.find("/no/such/echo-musl") != std::string::npos);
#endif
}

TEST_CASE("MUSL_SYSROOT needs crt objects and builtins", "[target][musl]")
{
#if defined(_WIN32)
    SKIP("MUSL_SYSROOT is a unix path");
#else
    EchoTests::ScopedProject project("musl", "incomplete-sysroot");
    std::error_code ec;
    std::filesystem::create_directories(project.root() / "include", ec);
    touch_empty(project.root() / "lib" / "libc.a");
    EnvRestore musl_sysroot_env("MUSL_SYSROOT", project.root().string().c_str());

    std::string error;
    std::vector<std::string> argv;
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-musl");
    REQUIRE(Compiler::musl_sysroot().empty());
    REQUIRE_FALSE(Compiler::append_codegen_target_args(argv, target, error));
    REQUIRE(error.find("MUSL_SYSROOT") != std::string::npos);
    REQUIRE(error.find(project.root().string()) != std::string::npos);
#endif
}

TEST_CASE("echoc build --target-libc musl is refused off linux", "[target][musl]")
{
#if defined(__linux__)
    SKIP("linux hosts resolve musl");
#else
    EchoTests::ScopedProject project("musl", "refused");
    EchoTests::write_file(project.root() / "a.eco", "echo 1;\n");
    const EchoTests::ProcessResult result =
        project.echoc(
            "build --target-os linux --target-libc musl -o a a.eco",
            project.root());
    REQUIRE(result.exit_code != 0);
    REQUIRE(result.output.find("Linux host") != std::string::npos);
#endif
}

TEST_CASE("echoc build --target-libc musl is refused for a lib", "[target][musl]")
{
#if !defined(__linux__)
    SKIP("linux hosts resolve musl");
#else
    EchoTests::ScopedProject project("musl", "lib");
    EchoTests::write_file(
        project.root() / "module.eco",
        "#[module: \"musllib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"g\" }]\n");
    EchoTests::write_file(project.root() / "src/g.eco", "public function f() : void {}\n");
    const EchoTests::ProcessResult result =
        project.echoc("build --target-libc musl", project.root());
    REQUIRE(result.exit_code != 0);
    REQUIRE(result.output.find("Musl Is A Command") != std::string::npos);
#endif
}
