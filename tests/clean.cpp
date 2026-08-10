#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <filesystem>
#include <sstream>
#include <string>

// what `echoc clean` removes, and - the half that matters - what it refuses to.
//
// removing a directory is the one destructive thing the compiler does, so every case here is really about
// the same question: has this directory proved it is echoc's. A `--build-dir` or a `#[build_dir:]` is a path
// somebody typed and has to carry the marker; the default is a name the compiler chose and does not.

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::line_starting_with;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("clean", name)
    {};
};


// a project whose application depends on one library, both of them manifest modules
void write_project(const ScopedProject &project)
{
    write_file(project.root() / "lib" / "module.eco",
        "#[module: \"cleanlib\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "lib" / "src" / "lib.eco",
        "namespace cleanlib;\n"
        "\n"
        "function twice(int32 $n) : int32\n"
        "{\n"
        "    return $n * 2;\n"
        "}\n");

    write_file(project.root() / "app" / "module.eco",
        "#[module: \"cleanapp\"]\n"
        "#[depends: \"../lib\"]\n"
        "#[sources: \"*.eco\"]\n");

    write_file(project.root() / "app" / "app.eco", "echo cleanlib::twice(21);\n");
}

};

TEST_CASE("clean removes the whole graph's build directories", "[clean]")
{
    ScopedProject project("whole_graph");
    write_project(project);

    const fs::path app_dir = project.root() / "app";

    REQUIRE(project.echoc("build -o out", app_dir).exit_code == 0);

    std::error_code ec;
    REQUIRE(fs::exists(project.root() / "lib" / "ecobuild", ec));
    REQUIRE(fs::exists(app_dir / "ecobuild", ec));

    const ProcessResult cleaned = project.echoc("clean", app_dir);

    REQUIRE(cleaned.exit_code == 0);

    // **a dependency's artifacts are as much this build's output as the entry's.** Leaving them would make
    // "so the next build starts from nothing" false for every module but one
    REQUIRE_FALSE(fs::exists(project.root() / "lib" / "ecobuild", ec));
    REQUIRE_FALSE(fs::exists(app_dir / "ecobuild", ec));

    REQUIRE(cleaned.output.find("removed 2 build directories.") != std::string::npos);
}

TEST_CASE("the build after a clean reuses nothing", "[clean]")
{
    ScopedProject project("full_miss");
    write_project(project);

    const fs::path app_dir = project.root() / "app";

    REQUIRE(project.echoc("build -o out", app_dir).exit_code == 0);

    // warm first, so the assertion below is about the clean rather than about a store that was never filled
    const ProcessResult warm = project.echoc("build -o out --explain cache", app_dir);
    REQUIRE(warm.output.find("cleanlib") != std::string::npos);

    REQUIRE(project.echoc("clean", app_dir).exit_code == 0);

    const ProcessResult cold = project.echoc("build -o out --explain cache", app_dir);

    REQUIRE(cold.exit_code == 0);

    std::istringstream stream(cold.output);
    std::string line;
    bool found = false;

    while (std::getline(stream, line)) {
        if (line.find("cleanlib") != std::string::npos) {
            found = true;
            REQUIRE(line.find("miss") != std::string::npos);
        }
    }

    REQUIRE(found);
}

TEST_CASE("clean leaves the standard library's store alone unless asked", "[clean]")
{
    ScopedProject project("stdlib_store");
    write_project(project);

    const fs::path app_dir = project.root() / "app";

    REQUIRE(project.echoc("build -o out", app_dir).exit_code == 0);

    SECTION("by default it says it kept it")
    {
        const ProcessResult cleaned = project.echoc("clean", app_dir);

        REQUIRE(cleaned.exit_code == 0);

        // it is the machine's rather than this project's - shared by every project on it, and the slowest
        // thing in any build to produce again. Saying so is what keeps it from reading as an omission
        const std::string line = line_starting_with(cleaned.output, "stdlib");
        REQUIRE(line.find("kept") != std::string::npos);
        REQUIRE(line.find("--with-stdlib") != std::string::npos);
    }

    SECTION("and --stdlib reaches it")
    {
        const ProcessResult cleaned = project.echoc("clean --with-stdlib", app_dir);

        REQUIRE(cleaned.exit_code == 0);
        REQUIRE(line_starting_with(cleaned.output, "stdlib").find("kept") == std::string::npos);
    }
}

TEST_CASE("clean refuses a directory echoc did not create", "[clean]")
{
    ScopedProject project("refuses_foreign");
    write_project(project);

    // under the name the library's module would take, holding something nothing of ours wrote
    write_file(project.build_dir() / "cleanlib" / "notes.txt", "mine\n");

    const ProcessResult cleaned = project.echoc(
        "clean --build-dir " + quoted(project.build_dir()), project.root() / "app");

    // **a refusal is not a skip.** The person asked for a build that starts from nothing and did not get
    // one, so reporting success would be a lie about what is still on disk
    REQUIRE(cleaned.exit_code == 1);
    REQUIRE(line_starting_with(cleaned.output, "cleanlib").find("refused") != std::string::npos);
    REQUIRE(cleaned.output.find("CACHEDIR.TAG") != std::string::npos);

    std::error_code ec;
    REQUIRE(fs::exists(project.build_dir() / "cleanlib" / "notes.txt", ec));
}

TEST_CASE("clean removes nothing on a dry run", "[clean]")
{
    ScopedProject project("dry_run");
    write_project(project);

    const fs::path app_dir = project.root() / "app";

    REQUIRE(project.echoc("build -o out", app_dir).exit_code == 0);

    const ProcessResult cleaned = project.echoc("clean --dry-run", app_dir);

    REQUIRE(cleaned.exit_code == 0);
    REQUIRE(line_starting_with(cleaned.output, "cleanlib").find("would remove") != std::string::npos);

    std::error_code ec;
    REQUIRE(fs::exists(project.root() / "lib" / "ecobuild", ec));
    REQUIRE(fs::exists(app_dir / "ecobuild", ec));
}

TEST_CASE("clean follows the manifest's own build directory", "[clean]")
{
    ScopedProject project("manifest_attribute");
    write_project(project);

    write_file(project.root() / "lib" / "module.eco",
        "#[module: \"cleanlib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[build_dir: \"target\"]\n");

    const fs::path app_dir = project.root() / "app";

    REQUIRE(project.echoc("build -o out", app_dir).exit_code == 0);

    std::error_code ec;
    REQUIRE(fs::exists(project.root() / "lib" / "target", ec));

    REQUIRE(project.echoc("clean", app_dir).exit_code == 0);

    // it reads the same manifests the build did, so a module that moved its artifacts is still reached
    REQUIRE_FALSE(fs::exists(project.root() / "lib" / "target", ec));
}

TEST_CASE("clean in a directory with no project says so", "[clean]")
{
    ScopedProject project("no_project");

    const ProcessResult cleaned = project.echoc("clean", project.root());

    // there is no manifest, so there is no build directory to have produced - not an error, but not
    // silence either
    REQUIRE(cleaned.exit_code == 0);
    REQUIRE(cleaned.output.find("module.eco") != std::string::npos);
}

TEST_CASE("clean names a directory an older echoc left behind", "[clean]")
{
    ScopedProject project("legacy_directory");
    write_project(project);

    const fs::path app_dir = project.root() / "app";

    REQUIRE(project.echoc("build -o out", app_dir).exit_code == 0);

    // what a checkout made before the rename still holds. It carries no marker, so nothing proves it is
    // ours - naming it is the whole of what can honestly be done with it
    write_file(project.root() / "lib" / ".echo" / "cleanlib-0000000000000000.o", "stale\n");

    const ProcessResult cleaned = project.echoc("clean", app_dir);

    REQUIRE(cleaned.exit_code == 0);
    REQUIRE(cleaned.output.find("older echoc") != std::string::npos);

    std::error_code ec;
    REQUIRE(fs::exists(project.root() / "lib" / ".echo", ec));
}
