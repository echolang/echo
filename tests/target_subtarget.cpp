#include <catch2/catch_test_macros.hpp>

#include <Compiler/TargetSubtarget.h>

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
    REQUIRE(error.empty());
}
