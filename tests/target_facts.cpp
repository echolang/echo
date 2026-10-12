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
    REQUIRE(Compiler::TargetFacts::is_known_operating_system("wasi"));
    REQUIRE_FALSE(Compiler::TargetFacts::is_known_operating_system("macos"));
}

TEST_CASE("wasi is emit-only so run and test refuse it", "[target]")
{
    REQUIRE(Compiler::TargetFacts::is_emit_only_operating_system("wasi"));
    REQUIRE_FALSE(Compiler::TargetFacts::is_emit_only_operating_system("ios"));
    REQUIRE_FALSE(Compiler::TargetFacts::is_emit_only_operating_system("linux"));
}

TEST_CASE("family is a closed axis derived from os", "[target]")
{
    REQUIRE(Compiler::TargetFacts::is_axis("family"));
    REQUIRE(Compiler::TargetFacts::is_known_family("darwin"));
    REQUIRE(Compiler::TargetFacts::is_known_family("linux"));
    REQUIRE(Compiler::TargetFacts::is_known_family("windows"));
    REQUIRE(Compiler::TargetFacts::is_known_family("wasi"));
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

    facts.operating_system = "wasi";
    REQUIRE(facts.family() == "wasi");

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

    // `linux` is a GNU predefined macro (the integer 1), so the identifier cannot be that
    const Compiler::TargetFacts linux_gnu = Compiler::TargetFacts::from_triple("x86_64-unknown-linux-gnu");
    REQUIRE(linux_gnu.operating_system == "linux");
    REQUIRE(linux_gnu.family() == "linux");
    REQUIRE(linux_gnu.architecture == "x86_64");

    const Compiler::TargetFacts windows = Compiler::TargetFacts::from_triple("x86_64-pc-windows-msvc");
    REQUIRE(windows.operating_system == "windows");
    REQUIRE(windows.family() == "windows");

    const Compiler::TargetFacts wasi = Compiler::TargetFacts::from_triple("wasm32-unknown-wasip1");
    REQUIRE(wasi.operating_system == "wasi");
    REQUIRE(wasi.family() == "wasi");
    REQUIRE(wasi.architecture == "wasm32");
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

    REQUIRE(Compiler::TargetFacts::resolve("wasi", "wasm32", {}, facts, error));
    REQUIRE(facts.operating_system == "wasi");
    REQUIRE(facts.family() == "wasi");
    REQUIRE(facts.architecture == "wasm32");
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

TEST_CASE("wasi implies wasm32 and wasm32 requires wasi", "[target]")
{
    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE(Compiler::TargetFacts::resolve("wasi", "", {}, facts, error));
    REQUIRE(facts.operating_system == "wasi");
    REQUIRE(facts.architecture == "wasm32");
    REQUIRE(facts.pointer_size() == 4);
    REQUIRE(facts.admits_wasm_surface());
    REQUIRE_FALSE(facts.threads);
    REQUIRE_FALSE(facts.dynlib);
    REQUIRE(facts.has_define("threads") == false);
    REQUIRE(facts.has_define("dynlib") == false);

    REQUIRE_FALSE(Compiler::TargetFacts::resolve("linux", "wasm32", {}, facts, error));
    REQUIRE(error.find("wasm32 is only emitted for wasi") != std::string::npos);

    REQUIRE_FALSE(Compiler::TargetFacts::resolve("wasi", "arm64", {}, facts, error));
    REQUIRE(error.find("wasi is wasm32") != std::string::npos);
}

TEST_CASE("host facts have threads and dynlib", "[target]")
{
    const Compiler::TargetFacts host = Compiler::TargetFacts::host();
    if (host.operating_system != "wasi") {
        REQUIRE(host.threads);
        REQUIRE(host.dynlib);
        REQUIRE(host.has_define("threads"));
        REQUIRE(host.has_define("dynlib"));
        REQUIRE_FALSE(host.admits_wasm_surface());
        REQUIRE(host.pointer_size() == 8);
    }
}

TEST_CASE("resolve refuses --define threads because threads is a capability", "[target]")
{
    Compiler::TargetFacts facts;
    std::string error;

    REQUIRE_FALSE(Compiler::TargetFacts::resolve("", "", { "threads" }, facts, error));
    REQUIRE(error.find("platform capability") != std::string::npos);

    REQUIRE_FALSE(Compiler::TargetFacts::resolve("", "", { "dynlib" }, facts, error));
    REQUIRE(error.find("platform capability") != std::string::npos);
}

TEST_CASE("cache_signature folds threads and dynlib", "[target]")
{
    Compiler::TargetFacts facts;
    facts.operating_system = "linux";
    facts.architecture = "arm64";
    facts.threads = true;
    facts.dynlib = true;

    const std::string with = facts.cache_signature();
    REQUIRE(with.find("threads=1;") != std::string::npos);
    REQUIRE(with.find("dynlib=1;") != std::string::npos);

    facts.threads = false;
    facts.dynlib = false;
    const std::string without = facts.cache_signature();
    REQUIRE(without.find("threads=0;") != std::string::npos);
    REQUIRE(without != with);
}
