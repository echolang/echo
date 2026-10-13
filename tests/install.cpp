#include <catch2/catch_test_macros.hpp>

#include <Compiler/Install.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "subprocess.h"

// `echoc install` as a subprocess
// a target is a thing a manifest declares, and every corpus case is loose sources
// HOME is this project's, ECHO_BIN_DIR is cleared, so nothing here can write into the
// real ~/.echo/bin of whoever runs the suite

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

typedef std::vector<std::pair<std::string, std::string>> Environment;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("install", name)
    {};

    fs::path home() const {
        return root() / "home";
    }

    // where an install with no `--dir` and no ECHO_BIN_DIR goes, under this project's HOME
    fs::path default_bin() const
    {
#if defined(_WIN32)
        return home() / "AppData" / "Local" / "echo" / "bin";
#else
        return home() / ".echo" / "bin";
#endif
    }

    Environment environment(const std::string &bin_dir = "") const
    {
        return {
            { "HOME", home().string() },
            { "LOCALAPPDATA", (home() / "AppData" / "Local").string() },
            { "ECHO_BIN_DIR", bin_dir }
        };
    }

    ProcessResult install(const std::string &args, const Environment &env) const {
        return echoc(args, root(), env);
    }

    ProcessResult install(const std::string &args = "install") const {
        return install(args, environment());
    }
};

// two programs over one shared function, each printing its own name
// `marked` is the target that says `install: true`, or empty when the manifest marks none
void write_two_target_project(const ScopedProject &project, const std::string &marked = "")
{
    const auto target = [&marked](const std::string &name) {
        return "#[target: exe { name: \"" + name + "\", entry: \"src/" + name + "_main.eco\""
               + (name == marked ? ", install: true" : "") + " }]\n";
    };

    write_file(project.root() / "module.eco",
        "#[module: \"twotarget\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        + target("clock")
        + target("serve"));

    write_file(project.root() / "src/shared.eco",
        "function banner(string $who) : void\n"
        "{\n"
        "    echo($who);\n"
        "}\n");

    write_file(project.root() / "src/clock_main.eco", "banner(\"CLOCK\");\n");
    write_file(project.root() / "src/serve_main.eco", "banner(\"SERVE\");\n");
}

bool runs_and_prints(const fs::path &binary, const std::string &expected)
{
    const ProcessResult ran = EchoTests::run_binary(binary);
    return ran.exit_code == 0 && ran.output.find(expected) != std::string::npos;
}

};

TEST_CASE("a manifest that marks nothing installs every exe it declares", "[install]")
{
    ScopedProject project("installs_every_exe");
    write_two_target_project(project);

    const ProcessResult installed = project.install();
    INFO(installed.output);
    REQUIRE(installed.exit_code == 0);

    REQUIRE(runs_and_prints(project.default_bin() / "clock", "CLOCK"));
    REQUIRE(runs_and_prints(project.default_bin() / "serve", "SERVE"));

    // the build itself is what `echoc build` leaves, and the install is a copy of it
    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild/clock"));
    REQUIRE(installed.output.find("installed clock -> ") != std::string::npos);
}

TEST_CASE("`install: true` narrows what an unnamed install takes", "[install]")
{
    ScopedProject project("installs_the_marked");
    write_two_target_project(project, "clock");

    const ProcessResult installed = project.install();
    INFO(installed.output);
    REQUIRE(installed.exit_code == 0);

    REQUIRE(EchoTests::file_exists(project.default_bin() / "clock"));
    REQUIRE_FALSE(EchoTests::file_exists(project.default_bin() / "serve"));
}

TEST_CASE("`--target` installs the one named, marked or not", "[install]")
{
    ScopedProject project("installs_the_named");
    write_two_target_project(project, "clock");

    const ProcessResult installed = project.install("install --target serve");
    INFO(installed.output);
    REQUIRE(installed.exit_code == 0);

    REQUIRE(runs_and_prints(project.default_bin() / "serve", "SERVE"));
    REQUIRE_FALSE(EchoTests::file_exists(project.default_bin() / "clock"));
}

TEST_CASE("a lib target is never installed", "[install]")
{
    ScopedProject project("skips_a_lib");

    write_file(project.root() / "module.eco",
        "#[module: \"both\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/main.eco", "echo 1;\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult installed = project.install();
    INFO(installed.output);
    REQUIRE(installed.exit_code == 0);

    REQUIRE(EchoTests::file_exists(project.default_bin() / "app"));

    // the only thing in there is the program
    size_t entries = 0;
    for ([[maybe_unused]] const fs::directory_entry &entry : fs::directory_iterator(project.default_bin())) {
        entries++;
    }
    REQUIRE(entries == 1);

    const ProcessResult named = project.install("install --target greeter");
    INFO(named.output);
    REQUIRE(named.exit_code != 0);
    REQUIRE(named.output.find("'greeter' is a lib target, and 'install' installs programs") != std::string::npos);
}

TEST_CASE("a module of only libs has nothing to install", "[install]")
{
    ScopedProject project("only_a_lib");

    write_file(project.root() / "module.eco",
        "#[module: \"greeter\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult installed = project.install();
    INFO(installed.output);
    REQUIRE(installed.exit_code != 0);
    REQUIRE(installed.output.find("'greeter' is a lib target, and 'install' installs programs")
        != std::string::npos);
    REQUIRE_FALSE(fs::exists(project.default_bin()));
}

TEST_CASE("a module that names no program has nothing to install", "[install]")
{
    ScopedProject project("no_program");

    write_file(project.root() / "module.eco",
        "#[module: \"nameless\"]\n"
        "#[sources: \"src/*.eco\"]\n");
    write_file(project.root() / "src/main.eco", "echo 1;\n");

    const ProcessResult installed = project.install();
    INFO(installed.output);
    REQUIRE(installed.exit_code != 0);
    REQUIRE(installed.output.find("declares no programs") != std::string::npos);

    // refused before anything was built or created
    REQUIRE_FALSE(fs::exists(project.default_bin()));
}

TEST_CASE("--dir beats ECHO_BIN_DIR, which beats the home directory", "[install]")
{
    ScopedProject project("directory_order");
    write_two_target_project(project, "clock");

    const fs::path from_env = project.root() / "from_env";
    const fs::path from_flag = project.root() / "from_flag";

    const ProcessResult by_env = project.install("install", project.environment(from_env.string()));
    INFO(by_env.output);
    REQUIRE(by_env.exit_code == 0);
    REQUIRE(EchoTests::file_exists(from_env / "clock"));
    REQUIRE_FALSE(fs::exists(project.default_bin()));

    const ProcessResult by_flag = project.install(
        "install --dir " + from_flag.string(),
        project.environment(from_env.string()));
    INFO(by_flag.output);
    REQUIRE(by_flag.exit_code == 0);
    REQUIRE(EchoTests::file_exists(from_flag / "clock"));
}

TEST_CASE("installing again replaces what was there", "[install]")
{
    ScopedProject project("replaces");
    write_two_target_project(project, "clock");

    REQUIRE(project.install().exit_code == 0);

    write_file(project.root() / "src/clock_main.eco", "banner(\"CLOCK TWO\");\n");

    const ProcessResult again = project.install();
    INFO(again.output);
    REQUIRE(again.exit_code == 0);
    REQUIRE(runs_and_prints(project.default_bin() / "clock", "CLOCK TWO"));
    REQUIRE(EchoTests::file_exists(project.default_bin() / "clock"));

    // the staged copy was renamed into place
    // a successful replace leaves no Windows `.old` sibling either
    for (const fs::directory_entry &entry : fs::directory_iterator(project.default_bin())) {
        INFO(entry.path().string());
        const std::string name = entry.path().filename().string();
        REQUIRE(name.find(".install-") == std::string::npos);
        REQUIRE(name.find(".old") == std::string::npos);
    }
}

TEST_CASE("an installed program is a release build unless asked otherwise", "[install]")
{
    ScopedProject project("release_by_default");

    write_file(project.root() / "module.eco",
        "#[module: \"checked\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"checked\", entry: \"src/main.eco\" }]\n");
    write_file(project.root() / "src/main.eco",
        "assert(false);\n"
        "echo(\"PAST\");\n");

    REQUIRE(project.install().exit_code == 0);
    REQUIRE(runs_and_prints(project.default_bin() / "checked", "PAST"));

    REQUIRE(project.install("install --debug").exit_code == 0);
    REQUIRE(EchoTests::run_binary(project.default_bin() / "checked").exit_code != 0);
}

TEST_CASE("`--dir` pointed at a build directory is refused", "[install]")
{
    ScopedProject project("not_into_ecobuild");
    write_two_target_project(project, "clock");

    const ProcessResult installed = project.install(
        "install --dir " + (project.root() / "ecobuild").string());
    INFO(installed.output);
    REQUIRE(installed.exit_code != 0);
    REQUIRE(installed.output.find("is a build directory") != std::string::npos);

    REQUIRE_FALSE(fs::exists(project.default_bin()));
}

TEST_CASE("a PATH that already holds the bin directory is not warned about", "[install]")
{
    ScopedProject project("on_path");
    write_two_target_project(project, "clock");

    Environment env = project.environment();
    const char *path = std::getenv("PATH");
#if defined(_WIN32)
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    env.push_back({ "PATH", project.default_bin().string() + sep + (path == nullptr ? "" : path) });

    const ProcessResult installed = project.install("install", env);
    INFO(installed.output);
    REQUIRE(installed.exit_code == 0);
    REQUIRE(installed.output.find("is not on your PATH") == std::string::npos);
}

TEST_CASE("a PATH that does not hold the bin directory is a warning", "[install]")
{
    ScopedProject project("off_path");
    write_two_target_project(project, "clock");

    const ProcessResult installed = project.install();
    INFO(installed.output);
    REQUIRE(installed.exit_code == 0);
    REQUIRE(installed.output.find("is not on your PATH") != std::string::npos);
    REQUIRE(installed.output.find("Not On PATH") != std::string::npos);
}

TEST_CASE("install_binary refuses a destination that is a directory and leaves it", "[install]")
{
    ScopedProject project("binary_refuses_a_directory");

    const fs::path built = project.root() / "built";
    write_file(built, "not a binary");
    fs::permissions(built, fs::perms::owner_all);

    const fs::path destination_dir = project.root() / "clock";
    fs::create_directories(destination_dir);
    write_file(destination_dir / "keep", "still here");

    std::string reason;
    REQUIRE_FALSE(Compiler::install_binary(built, project.root(), "clock", reason));
    REQUIRE(reason.find("is a directory") != std::string::npos);
    REQUIRE(EchoTests::file_exists(destination_dir / "keep"));

    for (const fs::directory_entry &entry : fs::directory_iterator(project.root())) {
        REQUIRE(entry.path().filename().string().find(".install-") == std::string::npos);
    }
}
