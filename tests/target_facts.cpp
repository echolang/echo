#include <catch2/catch_test_macros.hpp>

#include <Compiler/TargetFacts.h>

#include <string>

// **what a condition can see**, as a closed table: os, family, arch, and the flags. family is derived
// from os, so a test that mutates operating_system still sees the right family - that is the whole
// reason it is not a stored field

TEST_CASE("ios and android are known operating systems", "[target]")
{
    REQUIRE(Compiler::TargetFacts::is_known_operating_system("ios"));
    REQUIRE(Compiler::TargetFacts::is_known_operating_system("android"));
    REQUIRE_FALSE(Compiler::TargetFacts::is_known_operating_system("macos"));
}

TEST_CASE("family is a closed axis derived from os", "[target]")
{
    REQUIRE(Compiler::TargetFacts::is_axis("family"));
    REQUIRE(Compiler::TargetFacts::is_known_family("darwin"));
    REQUIRE(Compiler::TargetFacts::is_known_family("linux"));
    REQUIRE(Compiler::TargetFacts::is_known_family("windows"));
    REQUIRE_FALSE(Compiler::TargetFacts::is_known_family("unix"));
    REQUIRE_FALSE(Compiler::TargetFacts::is_known_family("apple"));

    Compiler::TargetFacts facts;

    facts.operating_system = "darwin";
    REQUIRE(facts.family() == "darwin");

    facts.operating_system = "ios";
    REQUIRE(facts.family() == "darwin");

    facts.operating_system = "linux";
    REQUIRE(facts.family() == "linux");

    facts.operating_system = "android";
    REQUIRE(facts.family() == "linux");

    facts.operating_system = "windows";
    REQUIRE(facts.family() == "windows");

    facts.operating_system = "";
    REQUIRE(facts.family().empty());
}

TEST_CASE("axis_names lists every axis the table owns", "[target]")
{
    REQUIRE(Compiler::TargetFacts::axis_names() == "os, arch, family");
}

TEST_CASE("from_triple maps Android before Linux and iOS before Darwin", "[target]")
{
    // four-component: Android is LLVM's environment, not its OS, so `aarch64-linux-android`
    // parses the middle token as vendor and never sets isAndroid()
    const Compiler::TargetFacts android = Compiler::TargetFacts::from_triple("aarch64-unknown-linux-android");
    REQUIRE(android.operating_system == "android");
    REQUIRE(android.family() == "linux");
    REQUIRE(android.architecture == "arm64");

    const Compiler::TargetFacts ios = Compiler::TargetFacts::from_triple("arm64-apple-ios");
    REQUIRE(ios.operating_system == "ios");
    REQUIRE(ios.family() == "darwin");
    REQUIRE(ios.architecture == "arm64");

    const Compiler::TargetFacts macos = Compiler::TargetFacts::from_triple("arm64-apple-macosx");
    REQUIRE(macos.operating_system == "darwin");
    REQUIRE(macos.family() == "darwin");

    const Compiler::TargetFacts linux = Compiler::TargetFacts::from_triple("x86_64-unknown-linux-gnu");
    REQUIRE(linux.operating_system == "linux");
    REQUIRE(linux.family() == "linux");
    REQUIRE(linux.architecture == "x86_64");

    const Compiler::TargetFacts windows = Compiler::TargetFacts::from_triple("x86_64-pc-windows-msvc");
    REQUIRE(windows.operating_system == "windows");
    REQUIRE(windows.family() == "windows");
}

TEST_CASE("shared_library_extension follows family, not os", "[target]")
{
    Compiler::TargetFacts facts;

    facts.operating_system = "ios";
    REQUIRE(facts.shared_library_extension() == ".dylib");

    facts.operating_system = "android";
    REQUIRE(facts.shared_library_extension() == ".so");

    facts.operating_system = "windows";
    REQUIRE(facts.shared_library_extension() == ".dll");
}

TEST_CASE("resolve accepts ios and android as a target os", "[target]")
{
    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, facts, error));
    REQUIRE(facts.operating_system == "ios");
    REQUIRE(facts.family() == "darwin");

    REQUIRE(Compiler::TargetFacts::resolve("android", "", {}, facts, error));
    REQUIRE(facts.operating_system == "android");
    REQUIRE(facts.family() == "linux");
}

TEST_CASE("resolve refuses a target os outside the vocabulary", "[target]")
{
    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE_FALSE(Compiler::TargetFacts::resolve("macos", "", {}, facts, error));
    REQUIRE(error.find("ios") != std::string::npos);
    REQUIRE(error.find("android") != std::string::npos);
}

TEST_CASE("resolve refuses --define family because family is an axis", "[target]")
{
    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE_FALSE(Compiler::TargetFacts::resolve("", "", { "family" }, facts, error));
    REQUIRE(error.find("condition axis") != std::string::npos);
}

TEST_CASE("cache_signature folds family so a recategorisation misses", "[target]")
{
    Compiler::TargetFacts facts;
    facts.operating_system = "ios";
    facts.architecture = "arm64";

    const std::string signature = facts.cache_signature();
    REQUIRE(signature.find("os=ios;") != std::string::npos);
    REQUIRE(signature.find("arch=arm64;") != std::string::npos);
    REQUIRE(signature.find("family=darwin;") != std::string::npos);
}

TEST_CASE("axis_equals refuses an unknown family value", "[target]")
{
    Compiler::TargetFacts facts;
    facts.operating_system = "linux";

    bool matched = true;
    std::string error;

    REQUIRE_FALSE(facts.axis_equals("family", "unix", matched, error));
    REQUIRE(error.find("unknown family 'unix'") != std::string::npos);
    REQUIRE(error.find("darwin") != std::string::npos);
}

TEST_CASE("axis_equals matches family independently of the specific os", "[target]")
{
    Compiler::TargetFacts facts;
    facts.operating_system = "ios";

    bool matched = false;
    std::string error;

    REQUIRE(facts.axis_equals("family", "darwin", matched, error));
    REQUIRE(matched);
    REQUIRE(error.empty());

    REQUIRE(facts.axis_equals("os", "ios", matched, error));
    REQUIRE(matched);

    REQUIRE(facts.axis_equals("os", "darwin", matched, error));
    REQUIRE_FALSE(matched);
}
