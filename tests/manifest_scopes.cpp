#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "subprocess.h"

// what a `#[target: ...] { ... }` scope promises: that what it says belongs to that target rather than to
// the module, and that a program which did not open it does not compile, key or link any of it.
//
// subprocess tests rather than corpus goldens for the reason tests/build_targets.cpp and
// tests/test_targets.cpp both give: every `.test` case appends its own `.eco` as a positional, so the
// harness always takes the "loose sources become the main module" branch - and a scope hangs off a target,
// which is by definition a thing a *manifest* declares. What the corpus can express is the refusals, and
// those live under tests_eco/errors/.
//
// the two halves worth keeping apart:
//
//   - a scope on a `test` target, which is the case it was built for. `tests/` joins the module when
//     `echoc test` builds it and is absent from `build` and `run` entirely
//   - a scope on an `exe` target, which is the case that had to move the module cache key. Two targets of
//     one module used to compile the identical source list by construction, so their objects were
//     interchangeable; with a scope they are not, and a key that did not say so would hand the second
//     target the first one's object with nothing anywhere saying it had

namespace
{

using EchoTests::ProcessResult;
using EchoTests::file_exists;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("manifest_scopes", name)
    {};
};

// a module whose tests live in a directory only the test target claims. `src/` holds the program and the
// function under test; `tests/` holds a test *and* a file the parser would refuse, which is what proves
// the directory was never read rather than merely contributing nothing
void write_scoped_test_project(const ScopedProject &project, bool with_a_broken_file)
{
    write_file(project.root() / "module.eco",
        "#[module: \"scoped\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: test] {\n"
        "    #[sources: \"tests/*.eco\"]\n"
        "}\n");

    write_file(project.root() / "src/main.eco", "echo(\"THE PROGRAM RAN\");\n");

    write_file(project.root() / "src/api.eco",
        "function doubled(int32 $n) : int32\n"
        "{\n"
        "    return $n * 2;\n"
        "}\n");

    write_file(project.root() / "tests/api_test.eco",
        "test doubling_doubles\n"
        "{\n"
        "    assert(doubled(21) == 42);\n"
        "}\n");

    if (with_a_broken_file) {
        write_file(project.root() / "tests/broken.eco", "this is not echo at all ][ {{{\n");
    }
}

};

TEST_CASE("a test target's scope contributes its sources to a test run", "[manifest_scopes]")
{
    ScopedProject project("test_scope_contributes");
    write_scoped_test_project(project, /*with_a_broken_file=*/false);

    const ProcessResult ran = project.echoc("test");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("doubling_doubles") != std::string::npos);

    // the module's own program did not run - a test run compiles the module and runs its tests
    REQUIRE(ran.output.find("THE PROGRAM RAN") == std::string::npos);
}

// **the load-bearing one.** A file a scope declares has to be genuinely *unread* by a build that did not
// open it, not merely stripped of its test blocks - so the directory holds a file that does not lex, which
// is the one difference a token filter cannot paper over
TEST_CASE("a build never reads a file only a test scope declares", "[manifest_scopes]")
{
    ScopedProject project("build_never_reads_it");
    write_scoped_test_project(project, /*with_a_broken_file=*/true);

    const ProcessResult built = project.echoc("build");
    INFO(built.output);

    REQUIRE(built.exit_code == 0);
    REQUIRE(file_exists(project.root() / "ecobuild/app"));

    // and the same directory does fail a test run, which is what says the file was there to be found
    const ProcessResult ran = project.echoc("test");
    INFO(ran.output);

    REQUIRE(ran.exit_code != 0);
}

// top-level code in a shared file is a refusal for a module declaring targets - and a scope's file is not
// a shared one. Without the exemption a fixture that runs a statement would refuse the whole build
TEST_CASE("top level code in a scoped source is not shared code", "[manifest_scopes]")
{
    ScopedProject project("scoped_top_level_code");

    write_file(project.root() / "module.eco",
        "#[module: \"fixtures\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: test] {\n"
        "    #[sources: \"tests/*.eco\"]\n"
        "}\n");

    write_file(project.root() / "src/main.eco", "echo(\"APP\");\n");

    write_file(project.root() / "tests/fixture_test.eco",
        "echo(\"A FIXTURE STATEMENT\");\n"
        "\n"
        "test it_runs\n"
        "{\n"
        "    assert(true);\n"
        "}\n");

    const ProcessResult ran = project.echoc("test");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("TopLevelCodeOutsideEntry") == std::string::npos);
}

// a module declaring *only* a test target has no entry at all, so every one of its files with top-level
// code used to answer "no target claims me". It still has to build, as the one program its module is
TEST_CASE("a module declaring only a test target still builds", "[manifest_scopes]")
{
    ScopedProject project("only_a_test_target");

    write_file(project.root() / "module.eco",
        "#[module: \"testsonly\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: test]\n");

    write_file(project.root() / "src/main.eco", "echo(\"IT BUILT\");\n");

    const ProcessResult built = project.echoc("build -o prog");
    INFO(built.output);

    REQUIRE(built.exit_code == 0);
    REQUIRE(file_exists(project.root() / "prog"));
}

// two `exe` targets, each made of a different set of files. This is the shape the module cache key had to
// grow a target axis for: without it both compute one hex, and the second target links the first's object
TEST_CASE("two exe targets with scopes compile different sources", "[manifest_scopes]")
{
    ScopedProject project("two_exe_scopes");

    write_file(project.root() / "module.eco",
        "#[module: \"twoways\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"one\", entry: \"one/main.eco\" }] {\n"
        "    #[sources: \"one/*.eco\"]\n"
        "}\n"
        "#[target: exe { name: \"two\", entry: \"two/main.eco\" }] {\n"
        "    #[sources: \"two/*.eco\"]\n"
        "}\n");

    write_file(project.root() / "src/shared.eco",
        "function shared() : string\n"
        "{\n"
        "    return 'SHARED';\n"
        "}\n");

    write_file(project.root() / "one/main.eco", "echo(shared()); echo(only_one());\n");
    write_file(project.root() / "one/only.eco",
        "function only_one() : string\n"
        "{\n"
        "    return 'ONLY ONE';\n"
        "}\n");

    write_file(project.root() / "two/main.eco", "echo(shared()); echo(only_two());\n");
    write_file(project.root() / "two/only.eco",
        "function only_two() : string\n"
        "{\n"
        "    return 'ONLY TWO';\n"
        "}\n");

    const ProcessResult built = project.echoc("build");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    const ProcessResult one = EchoTests::run_binary(project.root() / "ecobuild/one");
    INFO(one.output);

    REQUIRE(one.output.find("ONLY ONE") != std::string::npos);
    REQUIRE(one.output.find("ONLY TWO") == std::string::npos);

    const ProcessResult two = EchoTests::run_binary(project.root() / "ecobuild/two");
    INFO(two.output);

    REQUIRE(two.output.find("ONLY TWO") != std::string::npos);
    REQUIRE(two.output.find("ONLY ONE") == std::string::npos);
}

// **a dependency a scope declares is not compiled by a program that did not open the scope.** The whole of
// what a dev-dependency is, and the one contribution that changes which modules exist rather than which
// files one module holds
TEST_CASE("a scoped dependency is absent from a build that did not open it", "[manifest_scopes]")
{
    ScopedProject project("scoped_dependency");

    write_file(project.root() / "mocklib/module.eco",
        "#[module: \"mocklib\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "mocklib/src/mock.eco",
        "public function mock_value() : int32\n"
        "{\n"
        "    return 42;\n"
        "}\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: test] {\n"
        "    #[sources: \"tests/*.eco\"]\n"
        "    #[depends: \"../mocklib\"]\n"
        "}\n");

    write_file(project.root() / "app/src/api.eco",
        "function real_value() : int32\n"
        "{\n"
        "    return 42;\n"
        "}\n");

    write_file(project.root() / "app/tests/api_test.eco",
        "test the_mock_agrees\n"
        "{\n"
        "    assert(real_value() == mock_value());\n"
        "}\n");

    const ProcessResult ran = project.echoc("test --explain cache", project.root() / "app");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("the_mock_agrees") != std::string::npos);
    REQUIRE(ran.output.find("mocklib") != std::string::npos);

    // and a build reaches neither the module nor its symbols
    const ProcessResult built = project.echoc("build --explain cache -o prog", project.root() / "app");
    INFO(built.output);

    REQUIRE(built.exit_code == 0);
    REQUIRE(built.output.find("mocklib") == std::string::npos);
}

// **which modules a program compiles is a closure, not one step.** A module a scope names may be needed by
// another module a scope names: here `mocklib` is reached only through `testkit`, which is itself only
// reached through `app`'s test scope - and a second manifest naming `mocklib` from a scope of its own is
// what puts it on the droppable list in the first place
TEST_CASE("a scoped dependency's own dependencies are compiled too", "[manifest_scopes]")
{
    ScopedProject project("scoped_dependency_closure");

    write_file(project.root() / "mocklib/module.eco",
        "#[module: \"mocklib\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "mocklib/src/mock.eco",
        "public function mocked() : int32\n"
        "{\n"
        "    return 1;\n"
        "}\n");

    // reached only through app's test scope, and it needs mocklib at file scope
    write_file(project.root() / "testkit/module.eco",
        "#[module: \"testkit\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../mocklib\"]\n");

    write_file(project.root() / "testkit/src/kit.eco",
        "public function kit() : int32\n"
        "{\n"
        "    return mocked();\n"
        "}\n");

    // a second scope naming mocklib, which is what makes it a module something could decide to drop
    write_file(project.root() / "lib/module.eco",
        "#[module: \"lib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: test] {\n"
        "    #[depends: \"../mocklib\"]\n"
        "}\n");

    write_file(project.root() / "lib/src/lib.eco",
        "public function libfn() : int32\n"
        "{\n"
        "    return 2;\n"
        "}\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../lib\"]\n"
        "#[target: test] {\n"
        "    #[sources: \"tests/*.eco\"]\n"
        "    #[depends: \"../testkit\"]\n"
        "}\n");

    write_file(project.root() / "app/src/api.eco",
        "function appfn() : int32\n"
        "{\n"
        "    return libfn();\n"
        "}\n");

    write_file(project.root() / "app/tests/api_test.eco",
        "test the_kit_answers\n"
        "{\n"
        "    assert(kit() == 1);\n"
        "}\n");

    const ProcessResult ran = project.echoc("test", project.root() / "app");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("the_kit_answers") != std::string::npos);
}

TEST_CASE("a scoped #[requires:] is present for test and absent for build", "[manifest_scopes][packages]")
{
    ScopedProject project("scoped_requires");

    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: test] {\n"
        "    #[sources: \"tests/*.eco\"]\n"
        "    #[requires: \"libhello\" { version: \"^1.0\", source: git \"https://example.com/libhello\" }]\n"
        "}\n");

    write_file(project.root() / "app/src/api.eco",
        "function real_value() : int32\n"
        "{\n"
        "    return 42;\n"
        "}\n");

    write_file(project.root() / "app/tests/api_test.eco",
        "test the_package_agrees\n"
        "{\n"
        "    assert(real_value() == libhello::answer());\n"
        "}\n");

    write_file(project.root() / "app/vendor/libhello/module.eco",
        "#[module: \"libhello\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "app/vendor/libhello/src/hello.eco",
        "namespace libhello;\n"
        "public function answer() : int32 { return 42; }\n");

    const ProcessResult ran = project.echoc("test --explain cache", project.root() / "app");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("the_package_agrees") != std::string::npos);
    REQUIRE(ran.output.find("libhello") != std::string::npos);

    const ProcessResult built = project.echoc("build --explain cache -o prog", project.root() / "app");
    INFO(built.output);

    REQUIRE(built.exit_code == 0);
    REQUIRE(built.output.find("libhello") == std::string::npos);
}
