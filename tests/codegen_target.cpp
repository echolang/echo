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
    REQUIRE_FALSE(Compiler::resolve_codegen_target(facts, true, "", target, error));
    REQUIRE(error.find("--target-os ios") != std::string::npos);
    REQUIRE_FALSE(target.is_cross());
}

TEST_CASE("linux target-os stays the host triple even on build", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("linux", "", {}, facts, error));
    REQUIRE(Compiler::resolve_codegen_target(facts, false, "", target, error));
    REQUIRE_FALSE(target.is_cross());
    REQUIRE(target.triple.empty());
    REQUIRE(target.apple_sdk.empty());
}

TEST_CASE("ios build on Darwin is the simulator on this arch", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, facts, error));
    REQUIRE(Compiler::resolve_codegen_target(facts, false, "", target, error));

#if defined(__APPLE__)
    REQUIRE(target.is_cross());
    REQUIRE(target.triple == facts.architecture + "-apple-ios15.0-simulator");
    REQUIRE(target.apple_sdk == "iphonesimulator");
#else
    REQUIRE_FALSE(target.is_cross());
    REQUIRE(target.triple.empty());
#endif
}

TEST_CASE("ios-device on Darwin is iphoneos arm64", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, facts, error));
    const bool resolved = Compiler::resolve_codegen_target(facts, true, "", target, error);

#if defined(__APPLE__)
    REQUIRE(resolved);
    REQUIRE(target.triple == "arm64-apple-ios15.0");
    REQUIRE(target.apple_sdk == "iphoneos");
    REQUIRE(target.effective_triple() == target.triple);
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
    const bool resolved = Compiler::resolve_codegen_target(facts, true, "x86_64", target, error);

#if defined(__APPLE__)
    REQUIRE_FALSE(resolved);
    REQUIRE(error.find("arm64") != std::string::npos);
    REQUIRE(error.find("x86_64") != std::string::npos);
#else
    REQUIRE_FALSE(resolved);
#endif
}

TEST_CASE("ios-device with no arch override is arm64 for facts too", "[target]")
{
    REQUIRE(Compiler::facts_architecture(true, "") == "arm64");
    REQUIRE(Compiler::facts_architecture(true, "arm64") == "arm64");
    REQUIRE(Compiler::facts_architecture(true, "x86_64") == "x86_64");
    REQUIRE(Compiler::facts_architecture(false, "").empty());
    REQUIRE(Compiler::facts_architecture(false, "x86_64") == "x86_64");

    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve(
        "ios", Compiler::facts_architecture(true, ""), {}, facts, error));
    REQUIRE(facts.architecture == "arm64");

    Compiler::CodegenTarget target;
    const bool resolved = Compiler::resolve_codegen_target(facts, true, "", target, error);

#if defined(__APPLE__)
    REQUIRE(resolved);
    REQUIRE(target.triple == "arm64-apple-ios15.0");
#else
    REQUIRE_FALSE(resolved);
#endif
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
    REQUIRE(Compiler::resolve_codegen_target(facts, false, "wasm32", target, error));
    REQUIRE(target.is_cross());
    REQUIRE(target.is_wasm());
    REQUIRE(target.triple == "wasm32-unknown-wasip1");
    REQUIRE(target.apple_sdk.empty());
    REQUIRE(target.pointer_size() == 4);
    REQUIRE(target.effective_triple() == target.triple);
    REQUIRE(target.entry_symbol().has_value());
    REQUIRE(std::string(*target.entry_symbol()) == "__main_argc_argv");
    REQUIRE(target.exec_model == Compiler::ExecModel::t_command);
    REQUIRE(target.entry_arg_count() == 2);
    REQUIRE_FALSE(target.uses_pic());
    REQUIRE_FALSE(target.snapshots_bitcode());
    REQUIRE(std::string(target.output_extension()) == ".wasm");
    REQUIRE_FALSE(target.has_os_threads());
}

TEST_CASE("output_path appends the row suffix to a suffix-less name", "[target]")
{
    Compiler::CodegenTarget target;
    target.triple = Compiler::k_wasi_triple;
    target.wasm = true;
    target.pointer_bytes = 4;
    REQUIRE(target.output_path("hello") == std::filesystem::path("hello.wasm"));
    REQUIRE(target.output_path("hello.wasm") == std::filesystem::path("hello.wasm"));
    REQUIRE(target.output_path("hello.exe") == std::filesystem::path("hello.exe"));
}

TEST_CASE("host output_path is suffix-less except Windows .exe", "[target]")
{
    Compiler::CodegenTarget target;
#if defined(_WIN32)
    REQUIRE(target.output_path("hello") == std::filesystem::path("hello.exe"));
    REQUIRE(target.output_path("hello.exe") == std::filesystem::path("hello.exe"));
#else
    REQUIRE(target.output_path("hello") == std::filesystem::path("hello"));
#endif
}

TEST_CASE("wasi with a non-wasm32 arch override is refused", "[target]")
{
    Compiler::TargetFacts facts;
    Compiler::CodegenTarget target;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("wasi", "arm64", {}, facts, error));
    REQUIRE_FALSE(Compiler::resolve_codegen_target(facts, false, "arm64", target, error));
    REQUIRE(error.find("wasm32") != std::string::npos);
    REQUIRE(error.find("arm64") != std::string::npos);
}

TEST_CASE("is_wasm and pointer_size read the row columns", "[target]")
{
    Compiler::CodegenTarget target;
    target.triple = Compiler::k_wasi_triple;
    REQUIRE_FALSE(target.is_wasm());
    REQUIRE(target.pointer_size() == 8);

    target.wasm = true;
    target.pointer_bytes = 4;
    REQUIRE(target.is_wasm());
    REQUIRE(target.pointer_size() == 4);
}

TEST_CASE("host row keeps main and pic", "[target]")
{
    Compiler::CodegenTarget target;
    REQUIRE(target.pointer_size() == 8);
    REQUIRE_FALSE(target.is_wasm());
    REQUIRE(target.entry_symbol().has_value());
    REQUIRE(std::string(*target.entry_symbol()) == ECO_ENTRY_SYMBOL_NAME);
    REQUIRE(target.entry_arg_count() == 3);
    REQUIRE(target.uses_pic());
    REQUIRE(target.snapshots_bitcode());
    REQUIRE(target.output_extension()[0] == '\0');
    REQUIRE(target.has_os_threads());
}

TEST_CASE("a library has no entry symbol", "[target]")
{
    Compiler::CodegenTarget target;
    target.exec_model = Compiler::ExecModel::t_library;
    REQUIRE_FALSE(target.entry_symbol().has_value());
    REQUIRE(target.is_library());
    REQUIRE(target.is_native_library());
}

TEST_CASE("a native library takes the host shared-library suffix", "[target]")
{
    Compiler::CodegenTarget target;
    target.exec_model = Compiler::ExecModel::t_library;
#if defined(__APPLE__)
    REQUIRE(std::string(target.output_extension()) == ".dylib");
    REQUIRE(target.output_path("greeter") == std::filesystem::path("greeter.dylib"));
#elif defined(_WIN32)
    REQUIRE(std::string(target.output_extension()) == ".dll");
    REQUIRE(target.output_path("greeter") == std::filesystem::path("greeter.dll"));
#else
    REQUIRE(std::string(target.output_extension()) == ".so");
    REQUIRE(target.output_path("greeter") == std::filesystem::path("greeter.so"));
#endif
    REQUIRE(target.output_path("greeter.dylib") == std::filesystem::path("greeter.dylib"));
}

TEST_CASE("native library link args pass -shared", "[target]")
{
    Compiler::CodegenTarget target;
    target.exec_model = Compiler::ExecModel::t_library;
    std::vector<std::string> argv = { "clang" };
    std::string error;

    REQUIRE(Compiler::append_codegen_link_args(argv, target, error));
    REQUIRE(std::find(argv.begin(), argv.end(), "-shared") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-mexec-model=reactor") == argv.end());
}

TEST_CASE("a wasm library keeps the .wasm suffix", "[target]")
{
    Compiler::CodegenTarget target;
    target.triple = Compiler::k_wasi_triple;
    target.wasm = true;
    target.pointer_bytes = 4;
    target.exec_model = Compiler::ExecModel::t_library;
    REQUIRE(target.is_library());
    REQUIRE_FALSE(target.is_native_library());
    REQUIRE(std::string(target.output_extension()) == ".wasm");
    REQUIRE(target.output_path("plot") == std::filesystem::path("plot.wasm"));
}

TEST_CASE("wasi on build defaults facts arch to wasm32", "[target]")
{
    REQUIRE(Compiler::facts_architecture(false, "", "wasi", true) == "wasm32");
    REQUIRE(Compiler::facts_architecture(false, "", "wasi", false).empty());
    REQUIRE(Compiler::facts_architecture(false, "arm64", "wasi", true) == "arm64");
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
