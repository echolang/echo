#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

// where a build puts what it produces, and what it is allowed to put there.
//
// one question - `Compiler::BuildLayout`'s - asked from the outside, because the interesting half of it is
// what is on disk afterwards rather than what a function returned. The three answers that matter are the
// default, a manifest that names its own, and a `--build-dir` that overrides both; the fourth is that
// nothing lands anywhere else, which is the regression these exist for.

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("build_layout", name)
    {};
};

// a library any of these cases can depend on, optionally naming its own build directory
void write_library(const fs::path &directory, const std::string &module_name, const std::string &build_dir)
{
    write_file(directory / "module.eco",
        "#[module: \"" + module_name + "\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        + (build_dir.empty() ? "" : "#[build_dir: \"" + build_dir + "\"]\n"));

    write_file(directory / "src" / "lib.eco",
        "namespace " + module_name + ";\n"
        "\n"
        "function twice(int32 $n) : int32\n"
        "{\n"
        "    return $n * 2;\n"
        "}\n");
}

// the names directly inside a directory, so a case can say what a build left behind rather than probing
// for each thing it knows about - which is what makes "and nothing else" assertable at all
std::set<std::string> entries_of(const fs::path &directory)
{
    std::set<std::string> names;

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(directory, ec)) {
        names.insert(entry.path().filename().string());
    }

    return names;
}

bool holds_an_object(const fs::path &directory)
{
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(directory, ec)) {
        if (entry.path().extension() == ".o") {
            return true;
        }
    }

    return false;
}

};

TEST_CASE("a module's artifacts go to 'ecobuild' beside its manifest", "[layout]")
{
    ScopedProject project("default_directory");

    write_library(project.root() / "lib", "deflib", "");
    write_file(project.root() / "app" / "app.eco", "echo deflib::twice(21);\n");

    const ProcessResult built = project.echoc(
        "build -o out -m " + quoted(project.root() / "lib") + " app.eco", project.root() / "app");

    REQUIRE(built.exit_code == 0);

    // the artifacts are beside the library's own manifest, so they travel with the code they belong to
    REQUIRE(holds_an_object(project.root() / "lib" / "ecobuild"));

    // and the dot-directory an older echoc used is not created alongside it
    std::error_code ec;
    REQUIRE_FALSE(fs::exists(project.root() / "lib" / ".echo", ec));
}

TEST_CASE("every directory a build creates carries the cache marker", "[layout]")
{
    ScopedProject project("marker");

    write_library(project.root() / "lib", "marklib", "");
    write_file(project.root() / "app" / "app.eco", "echo marklib::twice(21);\n");

    REQUIRE(project.echoc(
        "build -o out -m " + quoted(project.root() / "lib") + " app.eco",
        project.root() / "app").exit_code == 0);

    // **the signature has to be the first 43 bytes**, or the backup tools that honour the Cache Directory
    // Tagging Specification do not recognise it - which is half of what writing the file buys
    std::ifstream marker(project.root() / "lib" / "ecobuild" / "CACHEDIR.TAG", std::ios::binary);
    REQUIRE(marker.good());

    std::string first;
    std::getline(marker, first);

    REQUIRE(first == "Signature: 8a477f597d28d172789f06886806bc55");
}

TEST_CASE("a manifest may name its own build directory", "[layout]")
{
    ScopedProject project("manifest_attribute");

    write_library(project.root() / "lib", "attrlib", "target");
    write_file(project.root() / "app" / "app.eco", "echo attrlib::twice(21);\n");

    SECTION("and it is honoured")
    {
        REQUIRE(project.echoc(
            "build -o out -m " + quoted(project.root() / "lib") + " app.eco",
            project.root() / "app").exit_code == 0);

        REQUIRE(holds_an_object(project.root() / "lib" / "target"));

        std::error_code ec;
        REQUIRE_FALSE(fs::exists(project.root() / "lib" / "ecobuild", ec));
    }

    SECTION("relative to the manifest, never to the working directory")
    {
        // built from somewhere else entirely, which is the whole of what a dependency's build looks like
        // from its consumer's side. A path resolved against the working directory would put a library's
        // artifacts wherever the person building it happened to be standing
        REQUIRE(project.echoc(
            "build -o out -m " + quoted(project.root() / "lib") + " " + quoted(project.root() / "app" / "app.eco"),
            project.root()).exit_code == 0);

        REQUIRE(holds_an_object(project.root() / "lib" / "target"));

        std::error_code ec;
        REQUIRE_FALSE(fs::exists(project.root() / "target", ec));
    }

    SECTION("and it governs the module that declared it, not its consumer")
    {
        write_file(project.root() / "app" / "module.eco",
            "#[module: \"consumer\"]\n"
            "#[depends: \"../lib\"]\n"
            "#[sources: \"*.eco\"]\n");

        REQUIRE(project.echoc("build -o out", project.root() / "app").exit_code == 0);

        REQUIRE(holds_an_object(project.root() / "lib" / "target"));

        // the consumer never said `target`, so it gets the default - one manifest's preference is not
        // inherited by whatever depends on it
        std::error_code ec;
        REQUIRE(fs::exists(project.root() / "app" / "ecobuild", ec));
        REQUIRE_FALSE(fs::exists(project.root() / "app" / "target", ec));
    }
}

TEST_CASE("--build-dir outranks the manifest", "[layout]")
{
    ScopedProject project("flag_wins");

    write_library(project.root() / "lib", "flaglib", "target");
    write_file(project.root() / "app" / "app.eco", "echo flaglib::twice(21);\n");

    const ProcessResult built = project.echoc(
        "build -o out -m " + quoted(project.root() / "lib")
            + " --build-dir " + quoted(project.build_dir()) + " app.eco",
        project.root() / "app");

    REQUIRE(built.exit_code == 0);

    // one directory for the whole build, with the module name as a subdirectory - which is what keeps two
    // modules from colliding when everything shares one root
    REQUIRE(holds_an_object(project.build_dir() / "flaglib"));

    std::error_code ec;
    REQUIRE_FALSE(fs::exists(project.root() / "lib" / "target", ec));
    REQUIRE_FALSE(fs::exists(project.root() / "lib" / "ecobuild", ec));
}

TEST_CASE("a build leaves nothing beside the binary it produced", "[layout]")
{
    // the regression this whole layout exists for: a build used to drop `out.<module>.o` and an `out.cc`
    // directory next to the executable, which nothing collected and no command removed
    ScopedProject project("no_litter");

    write_file(project.root() / "module.eco",
        "#[module: \"tidy\"]\n"
        "#[sources: \"src/*.eco\"]\n");
    write_file(project.root() / "src" / "app.eco", "echo 42;\n");

    REQUIRE(project.echoc("build -o out", project.root()).exit_code == 0);

    REQUIRE(entries_of(project.root()) == std::set<std::string>{ "module.eco", "src", "out", "ecobuild" });
}

TEST_CASE("a program with no project leaves only its binary", "[layout]")
{
    // there is no manifest to put a build directory beside, so the objects go to a per-process directory
    // under the system's temporary path - and that one is this process's to remove, since no later command
    // could ever know its name
    ScopedProject project("no_project");

    write_file(project.root() / "x.eco", "echo 7;\n");

    REQUIRE(project.echoc("build -o out x.eco", project.root()).exit_code == 0);

    REQUIRE(entries_of(project.root()) == std::set<std::string>{ "x.eco", "out" });
}

TEST_CASE("a build directory holding somebody else's work is refused", "[layout]")
{
    ScopedProject project("foreign_directory");

    write_library(project.root() / "lib", "foreignlib", "");
    write_file(project.root() / "app" / "app.eco", "echo foreignlib::twice(21);\n");

    // a directory under the name this module would take, with something in it that echoc did not write
    write_file(project.build_dir() / "foreignlib" / "notes.txt", "mine\n");

    const ProcessResult result = project.echoc(
        "build -o out -m " + quoted(project.root() / "lib")
            + " --build-dir " + quoted(project.build_dir()) + " app.eco",
        project.root() / "app");

    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("CACHEDIR.TAG") != std::string::npos);

    // **nothing was written and nothing was taken away.** An unwritable store is only slow; a store that
    // is somebody else's is a build pointed at the wrong place, and carrying on is how their work is lost
    std::error_code ec;
    REQUIRE(fs::exists(project.build_dir() / "foreignlib" / "notes.txt", ec));
    REQUIRE_FALSE(holds_an_object(project.build_dir()));
}

TEST_CASE("a build directory over the sources is refused at the manifest", "[layout]")
{
    ScopedProject project("build_dir_over_sources");

    write_file(project.root() / "app" / "app.eco", "echo 1;\n");

    // caught where the line number still is, rather than when a later `echoc clean` would empty it
    for (const auto &[named, expected] : {
             std::pair<std::string, std::string>{ ".", "own sources" },
             std::pair<std::string, std::string>{ "..", "own sources" },
             std::pair<std::string, std::string>{ "src/..", "own sources" },
             std::pair<std::string, std::string>{ "", "needs a directory name" } }) {
        // written out rather than through the helper, which reads an empty name as "declares none"
        write_file(project.root() / "lib" / "module.eco",
            "#[module: \"sourcelib\"]\n"
            "#[sources: \"src/*.eco\"]\n"
            "#[build_dir: \"" + named + "\"]\n");
        write_file(project.root() / "lib" / "src" / "lib.eco", "namespace sourcelib;\n");

        const ProcessResult result = project.echoc(
            "build -o out -m " + quoted(project.root() / "lib") + " app.eco", project.root() / "app");

        INFO("build_dir: '" << named << "' expecting: " << expected);

        REQUIRE(result.exit_code == 1);
        REQUIRE(result.output.find(expected) != std::string::npos);
        REQUIRE(result.output.find("module.eco:3") != std::string::npos);
    }
}
