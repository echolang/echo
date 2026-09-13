#include <catch2/catch_test_macros.hpp>

#include <Compiler/CodegenTarget.h>
#include <Compiler/TargetFacts.h>

#include <string>

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
