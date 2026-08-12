#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <filesystem>
#include <string>

// what `#[target:]` promises: that a module can hold several programs, that only the one being built runs,
// and that everything else in the module is shared by all of them.
//
// subprocess tests rather than corpus goldens because the corpus cannot express any of this. Every `.test`
// case appends its own `.eco` file as a positional and a `-o` beside it, so the harness always takes the
// "loose sources become the main module" branch - and a target is by definition a thing a *manifest*
// declares. The manifest *refusals* are goldens, under tests_eco/errors/, since those only need a bad
// module.eco reached with `-m`.

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("build_targets", name)
    {};
};

// a project with two programs over one shared function. **the two entries print different things**, which
// is the whole assertion this suite exists for: before targets, `main` was the concatenation of every file
// root of the module, so a second entry file would have run inside the first program rather than beside it
void write_two_target_project(const ScopedProject &project)
{
    write_file(project.root() / "module.eco",
        "#[module: \"twotarget\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"clock\", entry: \"src/clock_main.eco\" }]\n"
        "#[target: exe { name: \"serve\", entry: \"src/serve_main.eco\" }]\n");

    write_file(project.root() / "src/shared.eco",
        "function banner(string $who) : void\n"
        "{\n"
        "    echo($who);\n"
        "}\n");

    write_file(project.root() / "src/clock_main.eco", "banner(\"CLOCK\");\n");
    write_file(project.root() / "src/serve_main.eco", "banner(\"SERVE\");\n");
}

};

TEST_CASE("a build with no target named builds every one the manifest declares", "[targets]")
{
    ScopedProject project("builds_every_target");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild/clock"));
    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild/serve"));
}

TEST_CASE("each target runs its own entry file and nothing of the other's", "[targets]")
{
    ScopedProject project("entries_do_not_mix");
    write_two_target_project(project);

    REQUIRE(project.echoc("build").exit_code == 0);

    const ProcessResult clock = EchoTests::run_capturing(
        EchoTests::quoted(project.root() / "ecobuild/clock") + " 2>&1");
    const ProcessResult serve = EchoTests::run_capturing(
        EchoTests::quoted(project.root() / "ecobuild/serve") + " 2>&1");

    INFO("clock: " << clock.output << "\nserve: " << serve.output);

    // the shared function reached both, and neither ran the other's top level
    REQUIRE(clock.output.find("CLOCK") != std::string::npos);
    REQUIRE(clock.output.find("SERVE") == std::string::npos);
    REQUIRE(serve.output.find("SERVE") != std::string::npos);
    REQUIRE(serve.output.find("CLOCK") == std::string::npos);
}

TEST_CASE("--target builds the one named and leaves the others alone", "[targets]")
{
    ScopedProject project("one_target_only");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build --target clock");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild/clock"));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "ecobuild/serve"));
}

TEST_CASE("-o overrides where one target's binary goes", "[targets]")
{
    ScopedProject project("output_override");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build --target clock -o elsewhere");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    REQUIRE(EchoTests::file_exists(project.root() / "elsewhere"));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "ecobuild/clock"));
}

TEST_CASE("one path cannot name several binaries", "[targets]")
{
    ScopedProject project("output_for_several");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build -o everything");
    INFO(built.output);

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("names one file") != std::string::npos);
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "everything"));
}

TEST_CASE("run takes exactly one program, and says which there were", "[targets]")
{
    ScopedProject project("run_needs_one");
    write_two_target_project(project);

    const ProcessResult ran = project.echoc("run");
    INFO(ran.output);

    REQUIRE(ran.exit_code != 0);
    REQUIRE(ran.output.find("clock") != std::string::npos);
    REQUIRE(ran.output.find("serve") != std::string::npos);

    const ProcessResult named = project.echoc("run --target serve");
    INFO(named.output);
    REQUIRE(named.exit_code == 0);
    REQUIRE(named.output.find("SERVE") != std::string::npos);
    REQUIRE(named.output.find("CLOCK") == std::string::npos);
}

TEST_CASE("a target that was never declared is refused with the ones that were", "[targets]")
{
    ScopedProject project("unknown_target");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build --target nope");
    INFO(built.output);

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("no target called 'nope'") != std::string::npos);
    REQUIRE(built.output.find("clock, serve") != std::string::npos);
}

TEST_CASE("top level code in a file no target claims is refused", "[targets]")
{
    ScopedProject project("shared_top_level_code");
    write_two_target_project(project);

    // at the root of the *shared* file, which every target compiles and none of them runs
    write_file(project.root() / "src/shared.eco",
        "function banner(string $who) : void\n"
        "{\n"
        "    echo($who);\n"
        "}\n"
        "\n"
        "echo(\"this cannot run\");\n");

    const ProcessResult built = project.echoc("build");
    INFO(built.output);

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("TopLevelCodeOutsideEntry") != std::string::npos);
    REQUIRE(built.output.find("shared.eco") != std::string::npos);
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "ecobuild/clock"));
}

TEST_CASE("the other target's entry file is not refused for holding its own program", "[targets]")
{
    ScopedProject project("other_entry_is_not_shared");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build --target clock");
    INFO(built.output);

    // serve_main.eco is top-level code in a file this program does not run, and it is *not* the mistake
    // the case above is - it is the other program. Only a file no target claims is
    REQUIRE(built.exit_code == 0);
    REQUIRE(built.output.find("TopLevelCodeOutsideEntry") == std::string::npos);
}

TEST_CASE("a module with no target is the one program it always was", "[targets]")
{
    ScopedProject project("no_targets_declared");

    write_file(project.root() / "module.eco",
        "#[module: \"plain\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    // two file roots with code in both, which is the shape a target narrows and a target-less module
    // still concatenates - the behaviour every project compiled before targets existed relies on
    write_file(project.root() / "src/a.eco", "echo(\"A\");\n");
    write_file(project.root() / "src/b.eco", "echo(\"B\");\n");

    // and `-o` is still required, now refused where the manifest is known rather than by the parser
    const ProcessResult without_output = project.echoc("build");
    INFO(without_output.output);
    REQUIRE(without_output.exit_code != 0);
    REQUIRE(without_output.output.find("--output") != std::string::npos);

    const ProcessResult ran = project.echoc("run");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("A") != std::string::npos);
    REQUIRE(ran.output.find("B") != std::string::npos);
}

TEST_CASE("a dependency's targets are its own and reach no consumer", "[targets]")
{
    ScopedProject project("targets_do_not_travel");

    write_file(project.root() / "lib/module.eco",
        "#[module: \"libwithtargets\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"tool\", entry: \"src/tool_main.eco\" }]\n");

    write_file(project.root() / "lib/src/api.eco",
        "public function greet() : void\n"
        "{\n"
        "    echo(\"HELLO\");\n"
        "}\n");

    // the library's own program. A consumer must not run this, and must not inherit the target either
    write_file(project.root() / "lib/src/tool_main.eco", "echo(\"LIBTOOL\");\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"appofit\"]\n"
        "#[depends: \"../lib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/app_main.eco\" }]\n");

    write_file(project.root() / "app/src/app_main.eco", "greet();\n");

    const ProcessResult built = project.echoc("build", project.root() / "app");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    // the consumer built its own target and not the library's
    REQUIRE(EchoTests::file_exists(project.root() / "app/ecobuild/app"));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "app/ecobuild/tool"));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "lib/ecobuild/tool"));

    const ProcessResult ran = EchoTests::run_capturing(
        EchoTests::quoted(project.root() / "app/ecobuild/app") + " 2>&1");

    INFO(ran.output);
    REQUIRE(ran.output.find("HELLO") != std::string::npos);
    REQUIRE(ran.output.find("LIBTOOL") == std::string::npos);
}

TEST_CASE("two targets of one module share their dependencies' cached objects", "[targets]")
{
    ScopedProject project("targets_share_the_cache");

    write_file(project.root() / "lib/module.eco",
        "#[module: \"sharedlib\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "lib/src/api.eco",
        "public function greet(string $who) : void\n"
        "{\n"
        "    echo($who);\n"
        "}\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"twoofthem\"]\n"
        "#[depends: \"../lib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"one\", entry: \"src/one.eco\" }]\n"
        "#[target: exe { name: \"two\", entry: \"src/two.eco\" }]\n");

    write_file(project.root() / "app/src/one.eco", "greet(\"ONE\");\n");
    write_file(project.root() / "app/src/two.eco", "greet(\"TWO\");\n");

    const ProcessResult built = project.echoc("build --explain cache", project.root() / "app");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    // **the first target writes the library's object and the second reuses it**, which is the property
    // that makes a second target cost its own code rather than the whole project's. It is also the
    // condition under which a library object depending on its consumer would be served wrongly - see
    // todo/M12 - so this case is where that would first show
    REQUIRE(built.output.find("sharedlib") != std::string::npos);
    REQUIRE(built.output.find("hit") != std::string::npos);
}
