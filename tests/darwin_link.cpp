#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
#include <string>

#include "subprocess.h"

// the Darwin SDK version `ld` records in LC_BUILD_VERSION. cannot be a corpus golden:
// the version is a property of this machine's SDK, and a pipe-captured `otool` dump
// is not a program's output.
//
// without `-platform_version` the direct `ld` path copies the objects' build version,
// whose sdk is 0 / n/a. AppKit treats that as predating dark mode and forces light
// appearance. clang-linked `echoc` itself is fine; `echoc build` has always been
// wrong; `echoc run`'s `scratch/eco_run` is the path that made it visible.

#if defined(__APPLE__)

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::run_capturing;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("darwin_link", name)
    {};
};

void write_trivial_app(const ScopedProject &project)
{
    // a manifest so `run` puts the scratch binary at `ecobuild/scratch/eco_run`.
    // a loose `.eco` uses the per-process temp scratch instead
    write_file(project.root() / "module.eco", R"(
#[module: "app"]
#[sources: "*.eco"]
)");

    write_file(project.root() / "app.eco", R"(
echo 1;
)");
}

std::string build_version_sdk(const std::string &otool)
{
    bool in_block = false;
    std::istringstream stream(otool);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.find("LC_BUILD_VERSION") != std::string::npos) {
            in_block = true;
            continue;
        }

        if (!in_block) {
            continue;
        }

        std::istringstream fields(line);
        std::string key;
        std::string value;
        fields >> key >> value;

        if (key == "cmd") {
            break;
        }

        if (key == "sdk") {
            return value;
        }
    }

    return {};
}

bool sdk_version_at_least(const std::string &sdk, unsigned want_major, unsigned want_minor)
{
    unsigned major = 0;
    unsigned minor = 0;
    char dot = 0;
    std::istringstream version(sdk);
    if (!(version >> major)) {
        return false;
    }

    if (version >> dot && dot == '.') {
        version >> minor;
    }

    if (major != want_major) {
        return major > want_major;
    }

    return minor >= want_minor;
}

void require_recorded_sdk(const fs::path &binary)
{
    REQUIRE(EchoTests::file_exists(binary));

    const ProcessResult dump = run_capturing("otool -l " + quoted(binary));
    INFO(dump.output);
    REQUIRE(dump.exit_code == 0);

    const std::string sdk = build_version_sdk(dump.output);
    REQUIRE_FALSE(sdk.empty());
    REQUIRE(sdk != "n/a");
    REQUIRE(sdk_version_at_least(sdk, 10, 14));
}

};

TEST_CASE("echoc build records the Darwin SDK version", "[darwin_link]")
{
    ScopedProject project("build");
    write_trivial_app(project);

    const ProcessResult result = project.echoc("build -o app app.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    require_recorded_sdk(project.root() / "app");
}

TEST_CASE("echoc run records the Darwin SDK version", "[darwin_link]")
{
    ScopedProject project("run");
    write_trivial_app(project);

    // no positional source: a loose `.eco` uses the per-process temp scratch, which
    // is discarded after the program. a discovered manifest puts the runner at
    // `ecobuild/scratch/eco_run` and keeps it
    const ProcessResult result = project.echoc("run");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    require_recorded_sdk(project.root() / "ecobuild" / "scratch" / "eco_run");
}

// **what a Darwin executable exports is what dyld binds at launch.** Darwin exports every global by
// default, and a body shared between units is a weak one: each call to it is then bound by name before
// `main`, in every process, which a test run starts one of per test. nothing loads symbols out of a
// program but a plugin copying the hash seed, so that is the whole list - as it always was on ELF
TEST_CASE("a Darwin executable exports the hash seed and nothing else", "[darwin_link]")
{
    ScopedProject project("exports");

    write_file(project.root() / "module.eco", R"(
#[module: "app"]
#[sources: "*.eco"]
)");

    // a map hashes, so the seed exists; `array<int32>` is a generic instance, a body no one unit owns
    write_file(project.root() / "app.eco", R"(
map<string, int32> $ages = ['a' => 1];
array<int32> $list = [1, 2, 3];
echo $ages->count() + $list->count();
)");

    for (const std::string &link : { std::string(), std::string(" --link lib:z") }) {
        const ProcessResult result = project.echoc("build -o app" + link);
        INFO(result.output);
        REQUIRE(result.exit_code == 0);

        const ProcessResult ran = EchoTests::run_binary(project.root() / "app");
        REQUIRE(ran.exit_code == 0);
        REQUIRE(ran.output.find("4") != std::string::npos);

        const ProcessResult exported = run_capturing("nm -gU " + quoted(project.root() / "app"));
        INFO(exported.output);
        REQUIRE(exported.exit_code == 0);

        std::istringstream lines(exported.output);
        std::string address, kind, symbol;
        bool seed = false;

        while (lines >> address >> kind >> symbol) {
            if (symbol == "___eco_hash_seed") {
                seed = true;
                continue;
            }

            // the linker's own, in every executable
            REQUIRE(symbol == "__mh_execute_header");
        }

        REQUIRE(seed);
    }
}

#endif
