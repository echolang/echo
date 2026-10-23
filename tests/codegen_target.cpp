#include <catch2/catch_test_macros.hpp>

#include <AST/ASTValueType.h>
#include <Compiler/CodegenTarget.h>
#include <Compiler/HostTool.h>
#include <Compiler/TargetFacts.h>
#include <eco.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// **what this invocation emits**, as a row: Darwin `build --target-os ios` is a
// cross-compile, everything else stays the host. TargetFacts is not this question.
// `run --target-os ios` never calls this: the driver asks only on `build`, and
// tests_eco/conditional/os_branches_ios pins that the JIT stays on the host.

TEST_CASE("ios-device without --target-os ios is refused", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("", "", {}, facts, error));
    REQUIRE_FALSE(Compiler::resolve_codegen_target(facts, true, target, error));
    REQUIRE(error.find("--target-os ios") != std::string::npos);
    REQUIRE_FALSE(target.is_cross());
}

TEST_CASE("linux target-os stays the host triple even on build", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("linux", "", {}, facts, error));
    REQUIRE(Compiler::resolve_codegen_target(facts, false, target, error));
    REQUIRE_FALSE(target.is_cross());
    REQUIRE(target.triple().empty());
    REQUIRE(target.apple_sdk().empty());
}

TEST_CASE("ios build on Darwin is the simulator on this arch", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, facts, error));
    REQUIRE(Compiler::resolve_codegen_target(facts, false, target, error));

#if defined(__APPLE__)
    REQUIRE(target.is_cross());
    REQUIRE(target.triple() == facts.architecture + "-apple-ios15.0-simulator");
    REQUIRE(target.apple_sdk() == "iphonesimulator");
#else
    REQUIRE_FALSE(target.is_cross());
    REQUIRE(target.triple().empty());
#endif
}

TEST_CASE("ios-device on Darwin is iphoneos arm64", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, facts, error));
    const bool resolved = Compiler::resolve_codegen_target(facts, true, target, error);

#if defined(__APPLE__)
    REQUIRE(resolved);
    REQUIRE(target.triple() == "arm64-apple-ios15.0");
    REQUIRE(target.apple_sdk() == "iphoneos");
    REQUIRE(target.effective_triple() == target.triple());
#else
    REQUIRE_FALSE(resolved);
    REQUIRE(error.find("Darwin") != std::string::npos);
#endif
}

TEST_CASE("ios-device with a non-arm64 arch override is refused", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("ios", "x86_64", {}, facts, error));
    const bool resolved = Compiler::resolve_codegen_target(facts, true, target, error);

#if defined(__APPLE__)
    REQUIRE_FALSE(resolved);
    REQUIRE(error.find("arm64") != std::string::npos);
    REQUIRE(error.find("x86_64") != std::string::npos);
#else
    REQUIRE_FALSE(resolved);
#endif
}

namespace
{
    // the arch TargetFacts::resolve settles, or "refused: <sentence>"
    std::string facts_arch(bool ios_device, const std::string &arch, const std::string &os = {})
    {
        Compiler::TargetFacts facts;
        std::string error;
        if (!Compiler::TargetFacts::resolve(os, arch, {}, facts, error, ios_device)) {
            return "refused: " + error;
        }
        return facts.architecture;
    }
}

TEST_CASE("ios-device with no arch override is arm64 for facts too", "[target]")
{
    REQUIRE(facts_arch(true, "") == "arm64");
    REQUIRE(facts_arch(true, "arm64") == "arm64");
    REQUIRE(facts_arch(true, "x86_64") == "x86_64");
    REQUIRE(facts_arch(false, "") == Compiler::TargetFacts::host().architecture);
    REQUIRE(facts_arch(false, "x86_64") == "x86_64");

    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve(
        "ios", facts_arch(true, ""), {}, facts, error));
    REQUIRE(facts.architecture == "arm64");

    Compiler::CodegenTarget target;
    const bool resolved = Compiler::resolve_codegen_target(facts, true, target, error);

#if defined(__APPLE__)
    REQUIRE(resolved);
    REQUIRE(target.triple() == "arm64-apple-ios15.0");
#else
    REQUIRE_FALSE(resolved);
#endif
}

TEST_CASE("host_symbol_lookup is a row capability", "[target]")
{
    REQUIRE(Compiler::CodegenTarget::for_triple("x86_64-pc-windows-msvc").host_symbol_lookup()
        == Compiler::HostSymbolLookup::t_get_proc_address);

    const Compiler::CodegenTarget gnu =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-gnu");
    REQUIRE(gnu.host_symbol_lookup() == Compiler::HostSymbolLookup::t_dlsym);
    REQUIRE(gnu.rtld_default() == 0);

    const Compiler::CodegenTarget darwin =
        Compiler::CodegenTarget::for_triple("arm64-apple-macosx");
    REQUIRE(darwin.host_symbol_lookup() == Compiler::HostSymbolLookup::t_dlsym);
    REQUIRE(darwin.rtld_default() == -2);
}

TEST_CASE("effective_triple falls back to the host", "[target]")
{
    Compiler::CodegenTarget target;
    REQUIRE_FALSE(target.effective_triple().empty());
    REQUIRE_FALSE(target.is_cross());
}

TEST_CASE("wasi build is a real cross to wasm32-wasip1", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("wasi", "wasm32", {}, facts, error));
    REQUIRE(facts.operating_system == "wasi");
    REQUIRE(facts.family() == "wasi");
    REQUIRE(facts.architecture == "wasm32");
    REQUIRE(Compiler::resolve_codegen_target(facts, false, target, error));
    REQUIRE(target.is_cross());
    REQUIRE(target.is_wasm());
    REQUIRE(target.is_wasi());
    REQUIRE(target.triple() == "wasm32-unknown-wasip1");
    REQUIRE(target.apple_sdk().empty());
    REQUIRE(target.pointer_size() == 4);
    REQUIRE(target.effective_triple() == target.triple());
    REQUIRE(target.entry_symbol().has_value());
    REQUIRE(std::string(*target.entry_symbol()) == "__main_argc_argv");
    REQUIRE(target.exec_model() == Compiler::ExecModel::t_command);
    REQUIRE(target.entry_arg_count() == 2);
    REQUIRE_FALSE(target.uses_pic());
    REQUIRE_FALSE(target.snapshots_bitcode());
    REQUIRE(std::string(target.output_extension()) == ".wasm");
    REQUIRE_FALSE(target.has_os_threads());
}

TEST_CASE("output_path appends the row suffix to a suffix-less name", "[target]")
{
    const Compiler::CodegenTarget target = Compiler::CodegenTarget::wasi();
    REQUIRE(target.output_path("hello") == std::filesystem::path("hello.wasm"));
    REQUIRE(target.output_path("hello.wasm") == std::filesystem::path("hello.wasm"));
    REQUIRE(target.output_path("hello.exe") == std::filesystem::path("hello.exe"));
}

TEST_CASE("append_codegen_target_args dispatches on the row OS", "[target]")
{
    std::string error;

    std::vector<std::string> windows_argv;
    const Compiler::CodegenTarget windows =
        Compiler::CodegenTarget::for_triple("x86_64-pc-windows-msvc");
    REQUIRE(Compiler::append_codegen_target_args(windows_argv, windows, error));
#if defined(_WIN32)
    REQUIRE(std::find(windows_argv.begin(), windows_argv.end(), "-fms-runtime-lib=static")
        != windows_argv.end());
#else
    REQUIRE(std::find(windows_argv.begin(), windows_argv.end(), "-fms-runtime-lib=static")
        == windows_argv.end());
#endif

    // `linux` is a GNU predefined macro (the integer 1). target_facts.cpp spells
    // the same avoidance
    std::vector<std::string> linux_argv;
    const Compiler::CodegenTarget gnu =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-gnu");
    REQUIRE(Compiler::append_codegen_target_args(linux_argv, gnu, error));
    REQUIRE(std::find(linux_argv.begin(), linux_argv.end(), "-fms-runtime-lib=static")
        == linux_argv.end());
    REQUIRE(std::find(linux_argv.begin(), linux_argv.end(), "-isysroot") == linux_argv.end());
}

TEST_CASE("a windows link line carries the CRT flag once", "[target]")
{
    std::string error;
    std::vector<std::string> argv;
    const Compiler::CodegenTarget windows =
        Compiler::CodegenTarget::for_triple("x86_64-pc-windows-msvc");
    REQUIRE(Compiler::append_codegen_link_args(argv, windows, false, error));

    const auto crt_flags = std::count(
        argv.begin(), argv.end(), std::string("-fms-runtime-lib=static"));
#if defined(_WIN32)
    REQUIRE(crt_flags == 1);
    REQUIRE(std::find(argv.begin(), argv.end(), "-fuse-ld=lld") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-isystem") == argv.end());
#else
    REQUIRE(crt_flags == 0);
#endif
}

TEST_CASE("a command's suffix is the row's OS, not this process", "[target]")
{
    const Compiler::CodegenTarget windows =
        Compiler::CodegenTarget::for_triple("x86_64-pc-windows-msvc");
    REQUIRE(std::string(windows.output_extension()) == ".exe");
    REQUIRE(windows.output_path("hello") == std::filesystem::path("hello.exe"));
    REQUIRE(windows.output_path("hello.exe") == std::filesystem::path("hello.exe"));

    const Compiler::CodegenTarget gnu =
        Compiler::CodegenTarget::for_triple("x86_64-unknown-linux-gnu");
    REQUIRE(gnu.output_extension()[0] == '\0');
    REQUIRE(gnu.output_path("hello") == std::filesystem::path("hello"));
}

// a row's columns are read off its triple when it is built
TEST_CASE("a row reads its os and pointer width off its triple", "[target]")
{
    const Compiler::CodegenTarget wasi = Compiler::CodegenTarget::wasi();
    REQUIRE(wasi.os() == Compiler::TargetOs::t_wasi);
    REQUIRE(wasi.is_wasm());
    REQUIRE(wasi.is_wasi());
    REQUIRE(wasi.pointer_size() == 4);

    const Compiler::CodegenTarget unknown_wasm =
        Compiler::CodegenTarget::for_triple("wasm32-unknown-unknown");
    REQUIRE(unknown_wasm.is_wasm());
    REQUIRE_FALSE(unknown_wasm.is_wasi());
    REQUIRE_FALSE(unknown_wasm.has_os_threads());
    REQUIRE_FALSE(unknown_wasm.has_unload());
    REQUIRE(std::string(unknown_wasm.library_suffix()) == ".wasm");
    REQUIRE(unknown_wasm.library_file_name("greeter") == "greeter.wasm");

    const Compiler::CodegenTarget device = Compiler::CodegenTarget::ios_device();
    REQUIRE(device.os() == Compiler::TargetOs::t_ios);
    REQUIRE(device.is_darwin());
    REQUIRE(device.apple_sdk() == "iphoneos");
    REQUIRE(device.pointer_size() == 8);

    const Compiler::CodegenTarget host;
    REQUIRE_FALSE(host.is_wasm());
    REQUIRE_FALSE(host.is_wasi());
    REQUIRE(host.pointer_size() == ECO_TARGET_POINTER_SIZE);
}

TEST_CASE("a row's capabilities follow its os", "[target]")
{
    const Compiler::CodegenTarget wasi = Compiler::CodegenTarget::wasi();
    REQUIRE(wasi.environ_source() == Compiler::EnvironSource::t_wasilibc_get_environ);
    REQUIRE(wasi.stdio_unbuffering() == Compiler::StdioUnbuffering::t_wasi_libc_stdout);
    REQUIRE_FALSE(wasi.has_unload());
    REQUIRE_FALSE(wasi.entry_receives_envp());
    REQUIRE_FALSE(wasi.c_objects_use_fpic());

    const Compiler::CodegenTarget host;
    REQUIRE(host.has_unload());
    REQUIRE(host.entry_receives_envp());
#if defined(__APPLE__)
    REQUIRE(host.environ_source() == Compiler::EnvironSource::t_ns_get_environ);
    REQUIRE(host.stdio_unbuffering() == Compiler::StdioUnbuffering::t_none);
    REQUIRE(host.c_objects_use_fpic());
#elif defined(_WIN32)
    REQUIRE(host.environ_source() == Compiler::EnvironSource::t_none);
    REQUIRE(host.stdio_unbuffering() == Compiler::StdioUnbuffering::t_ucrt_stdout_stderr);
    REQUIRE_FALSE(host.c_objects_use_fpic());
#endif
}

TEST_CASE("host row keeps main and pic", "[target]")
{
    Compiler::CodegenTarget target;
    REQUIRE(target.pointer_size() == ECO_TARGET_POINTER_SIZE);
    REQUIRE_FALSE(target.is_wasm());
    REQUIRE(target.entry_symbol().has_value());
    REQUIRE(std::string(*target.entry_symbol()) == ECO_ENTRY_SYMBOL_NAME);
    REQUIRE(target.entry_arg_count() == 3);
    REQUIRE(target.uses_pic());
    REQUIRE(target.snapshots_bitcode());
#if defined(_WIN32)
    REQUIRE(std::string(target.output_extension()) == ".exe");
#else
    REQUIRE(target.output_extension()[0] == '\0');
#endif
    REQUIRE(target.has_os_threads());
}

TEST_CASE("a library has no entry symbol", "[target]")
{
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget().for_program(Compiler::ExecModel::t_library);
    REQUIRE_FALSE(target.entry_symbol().has_value());
    REQUIRE(target.is_library());
    REQUIRE(target.is_native_library());
}

TEST_CASE("reserved export names follow the row", "[target]")
{
    const Compiler::CodegenTarget host;
    Compiler::ReservedExportNames names = host.reserved_export_names();
    REQUIRE(names.entry.size() == 1);
    REQUIRE(std::string(names.entry[0]) == ECO_ENTRY_SYMBOL_NAME);
    REQUIRE(names.wasm_crt.empty());
    REQUIRE_FALSE(names.runtime.empty());
    REQUIRE(names.runtime_prefix == "__eco_");

    names = host.for_program(Compiler::ExecModel::t_library).reserved_export_names();
    REQUIRE(names.entry.size() == 1);
    REQUIRE(std::string(names.entry[0]) == ECO_ENTRY_SYMBOL_NAME);

    const Compiler::CodegenTarget wasi = Compiler::CodegenTarget::wasi();
    names = wasi.reserved_export_names();
    REQUIRE(names.entry.size() == 2);
    REQUIRE(std::string(names.entry[0]) == ECO_ENTRY_SYMBOL_NAME);
    REQUIRE(std::string(names.entry[1]) == "__main_argc_argv");
    REQUIRE_FALSE(names.wasm_crt.empty());

    names = wasi.for_program(Compiler::ExecModel::t_library).reserved_export_names();
    REQUIRE(names.entry.size() == 1);
    REQUIRE(std::string(names.entry[0]) == ECO_ENTRY_SYMBOL_NAME);
    REQUIRE_FALSE(names.wasm_crt.empty());
}

TEST_CASE("a native library takes the host shared-library suffix", "[target]")
{
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget().for_program(Compiler::ExecModel::t_library);
#if defined(__APPLE__)
    REQUIRE(std::string(target.output_extension()) == ".dylib");
    REQUIRE(target.output_path("greeter") == std::filesystem::path("greeter.dylib"));
    REQUIRE(target.library_file_name("greeter") == "libgreeter.dylib");
#elif defined(_WIN32)
    REQUIRE(std::string(target.output_extension()) == ".dll");
    REQUIRE(target.output_path("greeter") == std::filesystem::path("greeter.dll"));
    REQUIRE(target.library_file_name("greeter") == "greeter.dll");
#else
    REQUIRE(std::string(target.output_extension()) == ".so");
    REQUIRE(target.output_path("greeter") == std::filesystem::path("greeter.so"));
    REQUIRE(target.library_file_name("greeter") == "libgreeter.so");
#endif
    REQUIRE(target.output_path("greeter.dylib") == std::filesystem::path("greeter.dylib"));
}

TEST_CASE("library_file_name does not need exec_model set", "[target]")
{
    Compiler::CodegenTarget host;
#if defined(__APPLE__)
    REQUIRE(host.library_file_name("greeter") == "libgreeter.dylib");
#elif defined(_WIN32)
    REQUIRE(host.library_file_name("greeter") == "greeter.dll");
#else
    REQUIRE(host.library_file_name("greeter") == "libgreeter.so");
#endif
}

// `--target-os ios` off Darwin emits for the host and is still refused
TEST_CASE("an iOS target refuses a native library", "[target]")
{
    Compiler::TargetFacts ios;
    std::string error;
    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, ios, error));
    REQUIRE(Compiler::library_refusal(ios).has_value());

    Compiler::TargetFacts linux_facts;
    REQUIRE(Compiler::TargetFacts::resolve("linux", "", {}, linux_facts, error));
    REQUIRE_FALSE(Compiler::library_refusal(linux_facts).has_value());
}

TEST_CASE("native library link args pass -shared", "[target]")
{
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget().for_program(Compiler::ExecModel::t_library);
    std::vector<std::string> argv = { "clang" };
    std::string error;

    REQUIRE(Compiler::append_codegen_link_args(argv, target, false, error, "greeter.dylib"));
    REQUIRE(std::find(argv.begin(), argv.end(), "-shared") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-mexec-model=reactor") == argv.end());
    if (target.is_darwin()) {
        REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,-install_name,@rpath/greeter.dylib")
            != argv.end());
    }
    else if (target.is_windows()) {
        REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,/IMPLIB:greeter.lib") != argv.end());

        std::vector<std::string> with_dir = { "clang" };
        REQUIRE(Compiler::append_codegen_link_args(
            with_dir, target, false, error, "ecobuild/greeter.dll"));
        REQUIRE(std::find(with_dir.begin(), with_dir.end(), "-Wl,/IMPLIB:ecobuild/greeter.lib")
            != with_dir.end());
    }
    else {
        REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,-soname,greeter.dylib") != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,-z,defs") != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), "-Wl,-Bsymbolic-functions") != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), "-ldl") != argv.end());
    }
}

TEST_CASE("a wasm library keeps the .wasm suffix", "[target]")
{
    const Compiler::CodegenTarget target =
        Compiler::CodegenTarget::wasi().for_program(Compiler::ExecModel::t_library);
    REQUIRE(target.is_library());
    REQUIRE_FALSE(target.is_native_library());
    REQUIRE(std::string(target.output_extension()) == ".wasm");
    REQUIRE(target.output_path("plot") == std::filesystem::path("plot.wasm"));
    REQUIRE(target.library_file_name("plot") == "plot.wasm");
}

// wasi is wasm32 on every subcommand. `run --target-arch wasm32` would otherwise type usize as 32-bit
TEST_CASE("wasi and wasm32 imply each other for facts", "[target]")
{
    REQUIRE(facts_arch(false, "", "wasi") == "wasm32");
    REQUIRE(facts_arch(false, "wasm32", "wasi") == "wasm32");
    REQUIRE(facts_arch(false, "arm64", "wasi").starts_with("refused: wasi is wasm32"));
    REQUIRE(facts_arch(false, "wasm32").starts_with("refused: wasm32 is only emitted for wasi"));
    REQUIRE(facts_arch(false, "wasm32", "linux").starts_with("refused: wasm32 is only emitted for wasi"));
}

TEST_CASE("usize defaults to the host pointer size", "[target]")
{
    REQUIRE(AST::get_primitive_size(AST::ValueTypePrimitive::t_usize) == ECO_TARGET_POINTER_SIZE);
    REQUIRE(AST::get_primitive_size(AST::ValueTypePrimitive::t_isize) == ECO_TARGET_POINTER_SIZE);
    REQUIRE(AST::get_primitive_size(AST::ValueTypePrimitive::t_uint64) == 8);
}

TEST_CASE("PointerSizeScope restores the host width", "[target]")
{
    REQUIRE(AST::target_pointer_size() == ECO_TARGET_POINTER_SIZE);

    {
        AST::PointerSizeScope wasm32(4);
        REQUIRE(AST::get_primitive_size(AST::ValueTypePrimitive::t_usize) == 4);
        REQUIRE(AST::target_pointer_size() == 4);
    }

    REQUIRE(AST::target_pointer_size() == ECO_TARGET_POINTER_SIZE);
    REQUIRE(AST::get_primitive_size(AST::ValueTypePrimitive::t_usize) == ECO_TARGET_POINTER_SIZE);
}
