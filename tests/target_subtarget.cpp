#include <catch2/catch_test_macros.hpp>

#include <Compiler/CodegenTarget.h>
#include <Compiler/TargetSubtarget.h>

#include <algorithm>
#include <string>
#include <vector>

// **which CPU is the backend compiling for.** the Mac apple-m1 row is macOS, not every
// Darwin triple: iOS is Darwin to LLVM, and an iPhone is not an M1.

TEST_CASE("the apple-m1 baseline is macOS, not iOS", "[target]")
{
    REQUIRE(Compiler::baseline_subtarget_for("arm64-apple-macosx").cpu == "apple-m1");
    REQUIRE(Compiler::baseline_subtarget_for("arm64-apple-darwin").cpu == "apple-m1");
    REQUIRE(Compiler::baseline_subtarget_for("arm64-apple-ios").cpu == "generic");
    REQUIRE(Compiler::baseline_subtarget_for("arm64-apple-ios15.0").cpu == "generic");
    REQUIRE(Compiler::baseline_subtarget_for("arm64-apple-ios-simulator").cpu == "generic");
    REQUIRE(Compiler::baseline_subtarget_for("arm64-apple-ios15.0-simulator").cpu == "generic");
    REQUIRE(Compiler::baseline_subtarget_for("x86_64-apple-macosx").cpu == "generic");
    REQUIRE(Compiler::baseline_subtarget_for("wasm32-unknown-wasip1").cpu == "generic");
}

TEST_CASE("native is refused on a wasm triple", "[target]")
{
    Compiler::Subtarget subtarget;
    std::string error;

    REQUIRE_FALSE(Compiler::resolve_subtarget(
        "wasm32-unknown-wasip1", "native", "", subtarget, error));
    REQUIRE(error.find("native") != std::string::npos);
}

TEST_CASE("wasm32-unknown-wasip1 resolves as generic", "[target]")
{
    Compiler::Subtarget subtarget;
    std::string error;

    REQUIRE(Compiler::resolve_subtarget("wasm32-unknown-wasip1", "", "", subtarget, error));
    REQUIRE(subtarget.cpu == "generic");
    REQUIRE(subtarget.features == "+simd128");
    REQUIRE(error.empty());
}

TEST_CASE("C compile cpu flags come from append_codegen_cc_cpu_args", "[target]")
{
    Compiler::CodegenTarget target;
    target.triple = Compiler::k_wasi_triple;
    target.wasm = true;
    std::vector<std::string> argv;
    std::string error;

    REQUIRE(Compiler::append_codegen_cc_cpu_args(argv, target, "", "", error));
    REQUIRE(error.empty());
    REQUIRE(std::find(argv.begin(), argv.end(), "-target-cpu") == argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "generic") == argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "-target-feature") != argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "+simd128") != argv.end());
}

TEST_CASE("C compile omits -target-cpu generic on x86_64", "[target]")
{
    Compiler::CodegenTarget target;
    target.triple = "x86_64-unknown-linux-gnu";
    std::vector<std::string> argv;
    std::string error;

    REQUIRE(Compiler::append_codegen_cc_cpu_args(argv, target, "", "", error));
    REQUIRE(error.empty());
    REQUIRE(std::find(argv.begin(), argv.end(), "-target-cpu") == argv.end());
    REQUIRE(std::find(argv.begin(), argv.end(), "generic") == argv.end());
}
