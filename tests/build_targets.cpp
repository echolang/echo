#include <catch2/catch_test_macros.hpp>

#include "Compiler/TargetFacts.h"

#include <filesystem>
#include <sstream>
#include <string>

#include "subprocess.h"

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

void write_lib_only(const ScopedProject &project, const std::string &source)
{
    write_file(project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/lib.eco", source);
}

void write_exe_and_lib(const ScopedProject &project)
{
    write_file(project.root() / "module.eco",
        "#[module: \"both\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/main.eco", "echo 1;\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");
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

    const ProcessResult clock = EchoTests::run_binary(project.root() / "ecobuild/clock");
    const ProcessResult serve = EchoTests::run_binary(project.root() / "ecobuild/serve");

    INFO("clock: " << clock.output << "\nserve: " << serve.output);

    // the shared function reached both, and neither ran the other's top level
    REQUIRE(clock.output.find("CLOCK") != std::string::npos);
    REQUIRE(clock.output.find("SERVE") == std::string::npos);
    REQUIRE(serve.output.find("SERVE") != std::string::npos);
    REQUIRE(serve.output.find("CLOCK") == std::string::npos);
}

TEST_CASE("`--target` builds the one named and leaves the others alone", "[targets]")
{
    ScopedProject project("one_target_only");
    write_two_target_project(project);

    const ProcessResult built = project.echoc("build --target clock");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild/clock"));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "ecobuild/serve"));
}

TEST_CASE("`-o` overrides where one target's binary goes", "[targets]")
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

    const ProcessResult ran = EchoTests::run_binary(project.root() / "app/ecobuild/app");

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

TEST_CASE("a native lib target writes a shared library", "[targets]")
{
    ScopedProject project("native_lib");
    write_file(project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult built = project.echoc("build --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    const fs::path lib = project.root() / "ecobuild" / ("greeter" + ext);
    REQUIRE(EchoTests::file_exists(lib));
}

TEST_CASE("an iOS lib target is refused", "[targets]")
{
    ScopedProject project("ios_lib");
    write_file(project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult built = project.echoc("build --target-os ios --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("iOS") != std::string::npos);
}

TEST_CASE("run on a lib-only module is refused", "[targets]")
{
    ScopedProject project("lib_has_no_run");
    write_file(project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"mandelbrot\" }]\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function render() : int32 { return 1; }\n");

    const ProcessResult ran = project.echoc("run");
    INFO(ran.output);
    REQUIRE(ran.exit_code != 0);
    REQUIRE(ran.output.find("mandelbrot") != std::string::npos);
    REQUIRE(ran.output.find("lib target") != std::string::npos);
    REQUIRE(ran.output.find("--target-os wasi") == std::string::npos);

    const ProcessResult named = project.echoc("run --target mandelbrot");
    INFO(named.output);
    REQUIRE(named.exit_code != 0);
    REQUIRE(named.output.find("mandelbrot") != std::string::npos);
    REQUIRE(named.output.find("lib target") != std::string::npos);
    REQUIRE(named.output.find("--target-os wasi") == std::string::npos);
}

TEST_CASE("top level code in a lib file is refused", "[targets]")
{
    ScopedProject project("lib_top_level");
    write_file(project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"mandelbrot\" }]\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function render() : int32 { return 1; }\n"
        "echo 1;\n");

    const ProcessResult native = project.echoc("build --no-stdlib");
    INFO(native.output);
    REQUIRE(native.exit_code != 0);
    REQUIRE(native.output.find("TopLevelCodeOutsideEntry") != std::string::npos);

    const ProcessResult wasi = project.echoc("build --target-os wasi --no-stdlib");
    INFO(wasi.output);
    REQUIRE(wasi.exit_code != 0);
    REQUIRE(wasi.output.find("TopLevelCodeOutsideEntry") != std::string::npos);
}

std::string cache_hex(const std::string &line)
{
    std::istringstream fields(line);
    std::string name;
    std::string key;
    fields >> name >> key;
    return key;
}

TEST_CASE("an exe and a lib of one module have different cache keys", "[targets]")
{
    ScopedProject project("exe_lib_keys");
    write_file(project.root() / "module.eco",
        "#[module: \"both\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: lib { name: \"plug\" }]\n");
    write_file(project.root() / "src/main.eco", "echo 1;\n");
    write_file(project.root() / "src/plug.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult exe = project.echoc("build --target app --explain cache --no-stdlib");
    INFO(exe.output);
    REQUIRE(exe.exit_code == 0);

    const ProcessResult lib = project.echoc("build --target plug --explain cache --no-stdlib");
    INFO(lib.output);
    REQUIRE(lib.exit_code == 0);

    const std::string exe_key = cache_hex(EchoTests::line_starting_with(exe.output, "both"));
    const std::string lib_key = cache_hex(EchoTests::line_starting_with(lib.output, "both"));
    REQUIRE_FALSE(exe_key.empty());
    REQUIRE_FALSE(lib_key.empty());
    REQUIRE(exe_key != lib_key);
}

TEST_CASE("a native lib exports only its #[export]s", "[targets]")
{
    ScopedProject project("lib_visibility");
    write_file(project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult built = project.echoc("build --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    const fs::path lib = project.root() / "ecobuild" / ("greeter" + ext);
    REQUIRE(EchoTests::file_exists(lib));

    const ProcessResult nm = EchoTests::run_process(
        { "llvm-nm", "--extern-only", "--defined-only", lib.string() });
    INFO(nm.output);
    if (nm.exit_code != 0) {
        SKIP("llvm-nm not available");
    }

    REQUIRE(nm.output.find("add") != std::string::npos);
    REQUIRE(nm.output.find("__eco_alloc") == std::string::npos);
}

TEST_CASE("a host exe loads a native lib and calls an export", "[targets][dynlib]")
{
    ScopedProject project("dynlib_load");

    write_file(project.root() / "plugin/module.eco",
        "#[module: \"greeter\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "plugin/src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult plug = project.echoc("build --no-stdlib", project.root() / "plugin");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    const fs::path lib = project.root() / "plugin/ecobuild" / ("greeter" + ext);
    REQUIRE(EchoTests::file_exists(lib));

    write_file(project.root() / "host/module.eco",
        "#[module: \"host\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"host\", entry: \"src/main.eco\" }]\n");
    write_file(project.root() / "host/src/main.eco",
        "use std::dynlib;\n"
        "\n"
        "string $path = '" + lib.string() + "';\n"
        "dynlib::library $plug = guard dynlib::library::open($path) else ($e) {\n"
        "    die($e->message());\n"
        "};\n"
        "\n"
        "string $name = 'add';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else {\n"
        "    die('plugin has no add');\n"
        "};\n"
        "\n"
        "unsafe {\n"
        "    extern function<int32(int32, int32)> $add =\n"
        "        $raw:$ as extern function<int32(int32, int32)>;\n"
        "    echo $add(2, 3);\n"
        "}\n");

    const ProcessResult host = project.echoc("build", project.root() / "host");
    INFO(host.output);
    REQUIRE(host.exit_code == 0);

    const ProcessResult ran = EchoTests::run_binary(project.root() / "host/ecobuild/host");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("5") != std::string::npos);
}

TEST_CASE("a lib with no exports still writes the image", "[targets]")
{
    ScopedProject project("lib_no_exports");
    write_lib_only(project, "function hidden() : int32 { return 1; }\n");

    const ProcessResult built = project.echoc("build --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild" / ("greeter" + ext)));
}

TEST_CASE("-o on a lib applies the shared-library suffix", "[targets]")
{
    ScopedProject project("lib_output_suffix");
    write_lib_only(project,
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult built = project.echoc("build --no-stdlib -o elsewhere");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    REQUIRE(EchoTests::file_exists(project.root() / ("elsewhere" + ext)));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "ecobuild" / ("greeter" + ext)));
}

TEST_CASE("a module with an exe and a lib builds both when no target is named", "[targets]")
{
    ScopedProject project("exe_and_lib_all");
    write_exe_and_lib(project);

    const ProcessResult built = project.echoc("build --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild/app"));
    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild" / ("greeter" + ext)));
}

TEST_CASE("building the lib of a mixed module leaves the exe unbuilt", "[targets]")
{
    ScopedProject project("exe_and_lib_one");
    write_exe_and_lib(project);

    const ProcessResult built = project.echoc("build --no-stdlib --target greeter");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    REQUIRE(EchoTests::file_exists(project.root() / "ecobuild" / ("greeter" + ext)));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "ecobuild/app"));
}

TEST_CASE("the exe entry's top level is not refused when building the lib", "[targets]")
{
    ScopedProject project("lib_allows_exe_entry");
    write_exe_and_lib(project);

    const ProcessResult built = project.echoc("build --no-stdlib --target greeter");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);
    REQUIRE(built.output.find("TopLevelCodeOutsideEntry") == std::string::npos);
}

TEST_CASE("a custom export name is the symbol nm and dlsym see", "[targets]")
{
    ScopedProject project("lib_export_alias");
    write_lib_only(project,
        "#[export: \"sum\"]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult built = project.echoc("build --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    const fs::path lib = project.root() / "ecobuild" / ("greeter" + ext);
    REQUIRE(EchoTests::file_exists(lib));

    const ProcessResult nm = EchoTests::run_process(
        { "llvm-nm", "--extern-only", "--defined-only", lib.string() });
    INFO(nm.output);
    if (nm.exit_code != 0) {
        SKIP("llvm-nm not available");
    }

    REQUIRE(nm.output.find("sum") != std::string::npos);
    REQUIRE(nm.output.find("add") == std::string::npos);
}

TEST_CASE("an unexported helper is hidden from the dynamic symbol table", "[targets]")
{
    ScopedProject project("lib_hidden_helper");
    write_lib_only(project,
        "function helper(int32 $n) : int32 { return $n + 1; }\n"
        "\n"
        "#[export]\n"
        "public function bump(int32 $n) : int32 { return helper($n); }\n");

    const ProcessResult built = project.echoc("build --no-stdlib");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    const fs::path lib = project.root() / "ecobuild" / ("greeter" + ext);

    const ProcessResult nm = EchoTests::run_process(
        { "llvm-nm", "--extern-only", "--defined-only", lib.string() });
    INFO(nm.output);
    if (nm.exit_code != 0) {
        SKIP("llvm-nm not available");
    }

    REQUIRE(nm.output.find("bump") != std::string::npos);
    REQUIRE(nm.output.find("helper") == std::string::npos);
    REQUIRE(nm.output.find("__eco_alloc") == std::string::npos);
}
