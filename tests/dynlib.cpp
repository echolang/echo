#include <catch2/catch_test_macros.hpp>

#include "Compiler/TargetFacts.h"

#include <filesystem>
#include <string>

#include "subprocess.h"

// std::dynlib opens a native #[target: lib] and looks up #[export]s. subprocess
// rather than the corpus: a plugin is a second artifact, and a .test case cannot
// build one and then load it.

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("dynlib", name)
    {};
};

const std::string &lib_ext()
{
    static const std::string ext = Compiler::TargetFacts::host().shared_library_extension();
    return ext;
}

fs::path plugin_image(const ScopedProject &project)
{
    return project.root() / "plugin" / "ecobuild" / ("greeter" + lib_ext());
}

void write_plugin(const ScopedProject &project, const std::string &source)
{
    write_file(project.root() / "plugin/module.eco",
        "#[module: \"greeter\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    write_file(project.root() / "plugin/src/lib.eco", source);
}

void write_host(const ScopedProject &project, const std::string &source)
{
    write_file(project.root() / "host/module.eco",
        "#[module: \"host\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"host\", entry: \"src/main.eco\" }]\n");
    write_file(project.root() / "host/src/main.eco", source);
}

std::string open_plugin(const fs::path &lib)
{
    return
        "use std::dynlib;\n"
        "\n"
        "string $path = '" + EchoTests::echo_string_path(lib) + "';\n"
        "dynlib::library $plug = guard dynlib::library::open($path) else ($e) {\n"
        "    die($e->message());\n"
        "};\n";
}

ProcessResult build_plugin(const ScopedProject &project, const std::string &source)
{
    write_plugin(project, source);
    return project.echoc("build --no-stdlib", project.root() / "plugin");
}

ProcessResult build_and_run_host(const ScopedProject &project, const std::string &source)
{
    write_host(project, source);
    const ProcessResult built = project.echoc("build", project.root() / "host");
    if (built.exit_code != 0) {
        return built;
    }

    return EchoTests::run_binary(project.root() / "host/ecobuild/host");
}

};

TEST_CASE("dynlib::extension matches the host suffix", "[dynlib]")
{
    ScopedProject project("extension");
    write_host(project,
        "use std::dynlib;\n"
        "echo dynlib::extension();\n");

    const ProcessResult built = project.echoc("build", project.root() / "host");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const ProcessResult ran = EchoTests::run_binary(project.root() / "host/ecobuild/host");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find(lib_ext()) != std::string::npos);
}

TEST_CASE("open of a missing path is a dynerror", "[dynlib]")
{
    ScopedProject project("open_missing");
    const fs::path missing = project.root() / ("nosuch" + lib_ext());
    const ProcessResult ran = build_and_run_host(project,
        "use std::dynlib;\n"
        "\n"
        "string $path = '" + EchoTests::echo_string_path(missing) + "';\n"
        "guard dynlib::library::open($path) else {\n"
        "    echo 'failed';\n"
        "    return 0;\n"
        "};\n"
        "echo 'opened';\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("failed") != std::string::npos);
    REQUIRE(ran.output.find("opened") == std::string::npos);
}

TEST_CASE("a missing export name is null", "[dynlib]")
{
    ScopedProject project("missing_symbol");
    const ProcessResult plug = build_plugin(project,
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult ran = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'nope';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else {\n"
        "    echo 'missing';\n"
        "    return 0;\n"
        "};\n"
        "echo 'found';\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("missing") != std::string::npos);
    REQUIRE(ran.output.find("found") == std::string::npos);
}

TEST_CASE("#[export: \"name\"] is the symbol dynlib looks up", "[dynlib]")
{
    ScopedProject project("export_alias");
    const ProcessResult plug = build_plugin(project,
        "#[export: \"sum\"]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult by_alias = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'sum';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else {\n"
        "    die('plugin has no sum');\n"
        "};\n"
        "unsafe {\n"
        "    extern function<int32(int32, int32)> $add =\n"
        "        $raw:$ as extern function<int32(int32, int32)>;\n"
        "    echo $add(2, 3);\n"
        "}\n");
    INFO(by_alias.output);
    REQUIRE(by_alias.exit_code == 0);
    REQUIRE(by_alias.output.find("5") != std::string::npos);
}

TEST_CASE("the Echo name is not exported when #[export] renamed it", "[dynlib]")
{
    ScopedProject project("export_alias_hides_echo_name");
    const ProcessResult plug = build_plugin(project,
        "#[export: \"sum\"]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult by_echo_name = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'add';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else {\n"
        "    echo 'hidden';\n"
        "    return 0;\n"
        "};\n"
        "echo 'leaked';\n");
    INFO(by_echo_name.output);
    REQUIRE(by_echo_name.exit_code == 0);
    REQUIRE(by_echo_name.output.find("hidden") != std::string::npos);
    REQUIRE(by_echo_name.output.find("leaked") == std::string::npos);
}

TEST_CASE("two exports of one lib are independently callable", "[dynlib]")
{
    ScopedProject project("two_exports");
    const ProcessResult plug = build_plugin(project,
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n"
        "\n"
        "#[export]\n"
        "public function mul(int32 $a, int32 $b) : int32 { return $a * $b; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult ran = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $add_name = 'add';\n"
        "string $mul_name = 'mul';\n"
        "ptr<uint8> $add_raw = guard $plug->symbol($add_name) else { die('no add'); };\n"
        "ptr<uint8> $mul_raw = guard $plug->symbol($mul_name) else { die('no mul'); };\n"
        "unsafe {\n"
        "    extern function<int32(int32, int32)> $add =\n"
        "        $add_raw:$ as extern function<int32(int32, int32)>;\n"
        "    extern function<int32(int32, int32)> $mul =\n"
        "        $mul_raw:$ as extern function<int32(int32, int32)>;\n"
        "    echo $add(2, 3);\n"
        "    echo $mul(2, 3);\n"
        "}\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("5") != std::string::npos);
    REQUIRE(ran.output.find("6") != std::string::npos);
}

TEST_CASE("an unexported helper is reachable through an export, not through dlsym", "[dynlib]")
{
    ScopedProject project("hidden_helper");
    const ProcessResult plug = build_plugin(project,
        "function helper(int32 $n) : int32 { return $n + 1; }\n"
        "\n"
        "#[export]\n"
        "public function bump(int32 $n) : int32 { return helper($n); }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult via_export = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'bump';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else { die('no bump'); };\n"
        "unsafe {\n"
        "    extern function<int32(int32)> $bump =\n"
        "        $raw:$ as extern function<int32(int32)>;\n"
        "    echo $bump(3);\n"
        "}\n");
    INFO(via_export.output);
    REQUIRE(via_export.exit_code == 0);
    REQUIRE(via_export.output.find("4") != std::string::npos);

    const ProcessResult via_dlsym = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'helper';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else {\n"
        "    echo 'hidden';\n"
        "    return 0;\n"
        "};\n"
        "echo 'leaked';\n");
    INFO(via_dlsym.output);
    REQUIRE(via_dlsym.exit_code == 0);
    REQUIRE(via_dlsym.output.find("hidden") != std::string::npos);
    REQUIRE(via_dlsym.output.find("leaked") == std::string::npos);
}

TEST_CASE("symbol after close is null", "[dynlib]")
{
    ScopedProject project("close_then_symbol");
    const ProcessResult plug = build_plugin(project,
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult ran = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'add';\n"
        "$plug->close();\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else {\n"
        "    echo 'closed';\n"
        "    return 0;\n"
        "};\n"
        "echo 'still';\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("closed") != std::string::npos);
    REQUIRE(ran.output.find("still") == std::string::npos);
}

TEST_CASE("close twice is safe", "[dynlib]")
{
    ScopedProject project("close_twice");
    const ProcessResult plug = build_plugin(project,
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult ran = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "$plug->close();\n"
        "$plug->close();\n"
        "echo 'ok';\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("ok") != std::string::npos);
}

TEST_CASE("a plugin export can write through a pointer the host allocated", "[dynlib]")
{
    ScopedProject project("ptr_out");
    const ProcessResult plug = build_plugin(project,
        "#[export]\n"
        "public function store(ptr<int32> $p, int32 $v) : void { $p = $v; }\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult ran = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "string $name = 'store';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else { die('no store'); };\n"
        "int32 $n = 0;\n"
        "unsafe {\n"
        "    extern function<void(ptr<int32>, int32)> $store =\n"
        "        $raw:$ as extern function<void(ptr<int32>, int32)>;\n"
        "    $store(&$n, 9);\n"
        "}\n"
        "echo $n;\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("9") != std::string::npos);
}

TEST_CASE("a host function pointer can be passed into a plugin export", "[dynlib]")
{
    ScopedProject project("host_callback");
    const ProcessResult plug = build_plugin(project,
        "#[export]\n"
        "public function apply(extern function<int32(int32)> $f, int32 $x) : int32\n"
        "{\n"
        "    return $f($x);\n"
        "}\n");
    INFO(plug.output);
    REQUIRE(plug.exit_code == 0);

    const ProcessResult ran = build_and_run_host(project,
        open_plugin(plugin_image(project)) +
        "function twice(int32 $n) : int32 { return $n * 2; }\n"
        "\n"
        "string $name = 'apply';\n"
        "ptr<uint8> $raw = guard $plug->symbol($name) else { die('no apply'); };\n"
        "unsafe {\n"
        "    extern function<int32(extern function<int32(int32)>, int32)> $apply =\n"
        "        $raw:$ as extern function<int32(extern function<int32(int32)>, int32)>;\n"
        "    echo $apply(&twice, 21);\n"
        "}\n");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("42") != std::string::npos);
}
