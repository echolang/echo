#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <filesystem>
#include <string>

// what `#[target: test]` promises: that a module can name selections of its own tests, that a bare
// `echoc test` runs all of them anyway, and that a test target is invisible to `build` and to `run`.
//
// subprocess tests rather than corpus goldens for the reason tests/build_targets.cpp gives about `#[target:
// exe]`: every `.test` case appends its own `.eco` file as a positional, so the harness always takes the
// "loose sources become the main module" branch - and a target is by definition a thing a *manifest*
// declares. What the corpus *can* express is under tests_eco/tests/, which is everything about a test that
// does not need a manifest.
//
// it also pins the half of the design a single-module case cannot see at all: **a dependency's tests are not
// this invocation's**. Only the modules echoc was pointed at compile their test blocks, so a library's tests
// stay the library's until somebody points at it.

namespace
{

using EchoTests::ProcessResult;
using EchoTests::file_exists;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("test_targets", name)
    {};
};

// a project declaring two test targets over two groups, plus a program of its own. The program's `echo` is
// what says a test run compiled the module and ran none of it
void write_grouped_project(const ScopedProject &project)
{
    write_file(project.root() / "module.eco",
        "#[module: \"grouped\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: test]\n"
        "#[target: test { name: \"quick\", groups: [\"fast\"] }]\n");

    write_file(project.root() / "src/main.eco", "echo(\"THE PROGRAM RAN\");\n");

    write_file(project.root() / "src/suite.eco",
        "#[group: \"fast\"]\n"
        "test a_fast_one\n"
        "{\n"
        "    assert(1 + 1 == 2);\n"
        "}\n"
        "\n"
        "#[group: \"slow\"]\n"
        "test a_slow_one\n"
        "{\n"
        "    assert(2 + 2 == 4);\n"
        "}\n");
}

};

TEST_CASE("a bare test run runs every test the module has", "[test_targets]")
{
    ScopedProject project("runs_them_all");
    write_grouped_project(project);

    const ProcessResult ran = project.echoc("test");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("a_fast_one") != std::string::npos);
    REQUIRE(ran.output.find("a_slow_one") != std::string::npos);

    // **the module's own program did not run.** A test asked for is a test, not a test after the application
    // it sits beside - which is the whole of what CodegenContext::test_mode decides
    REQUIRE(ran.output.find("THE PROGRAM RAN") == std::string::npos);
}

TEST_CASE("a declared test target narrows only when it is named", "[test_targets]")
{
    ScopedProject project("named_target_narrows");
    write_grouped_project(project);

    const ProcessResult ran = project.echoc("test --target quick");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("a_fast_one") != std::string::npos);
    REQUIRE(ran.output.find("a_slow_one") == std::string::npos);
}

// **a saved selection is not one in force.** A module declaring a bare `#[target: test]` beside a narrow one
// would otherwise have `echoc test` mean the union of their narrowings, and the union of "everything" and
// "the fast ones" is not something the unnamed target can contribute to
TEST_CASE("an unnamed test target does not narrow a bare test run", "[test_targets]")
{
    ScopedProject project("unnamed_does_not_narrow");
    write_grouped_project(project);

    const ProcessResult bare = project.echoc("test");
    const ProcessResult named = project.echoc("test --target tests");

    INFO("bare: " << bare.output << "\nnamed: " << named.output);

    REQUIRE(bare.output.find("a_slow_one") != std::string::npos);
    REQUIRE(named.output.find("a_slow_one") != std::string::npos);
}

TEST_CASE("a test target is not a program, and build never builds one", "[test_targets]")
{
    ScopedProject project("build_ignores_tests");
    write_grouped_project(project);

    const ProcessResult built = project.echoc("build");
    INFO(built.output);

    REQUIRE(built.exit_code == 0);

    // the one program it declares, and nothing under either test target's name
    REQUIRE(file_exists(project.root() / "ecobuild/app"));
    REQUIRE_FALSE(file_exists(project.root() / "ecobuild/tests"));
    REQUIRE_FALSE(file_exists(project.root() / "ecobuild/quick"));
}

TEST_CASE("a program's target cannot be named to a test run, nor a test target to a build", "[test_targets]")
{
    ScopedProject project("kinds_do_not_mix");
    write_grouped_project(project);

    const ProcessResult tested = project.echoc("test --target app");
    const ProcessResult built = project.echoc("build --target quick");

    INFO("test --target app: " << tested.output << "\nbuild --target quick: " << built.output);

    REQUIRE(tested.exit_code != 0);
    REQUIRE(tested.output.find("No Such Target") != std::string::npos);

    // and the refusal names what there *was*, which for a test run is the test targets only
    REQUIRE(tested.output.find("tests, quick") != std::string::npos);

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("No Such Target") != std::string::npos);
}

// **the modules an invocation pointed at compile their tests, and nothing below them does.**
//
// the alternative - every module in the graph - would have a project's `echoc test` parse and type-check the
// standard library's tests, and then either run somebody else's or drop them having paid for them
TEST_CASE("a dependency's tests are not this invocation's", "[test_targets]")
{
    ScopedProject project("dependency_tests_stay_put");

    write_file(project.root() / "lib/module.eco",
        "#[module: \"dep\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "lib/src/lib.eco",
        "public function doubled(int32 $n) : int32\n"
        "{\n"
        "    return $n * 2;\n"
        "}\n"
        "\n"
        "test the_librarys_own\n"
        "{\n"
        "    assert(doubled(2) == 4);\n"
        "}\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../lib\"]\n");

    write_file(project.root() / "app/src/app.eco",
        "test the_applications_own\n"
        "{\n"
        "    assert(doubled(3) == 6);\n"
        "}\n");

    const ProcessResult app = EchoTests::run_process({
        ECHOC_BINARY, "test", "-m", (project.root() / "app").string() });

    INFO(app.output);
    REQUIRE(app.exit_code == 0);
    REQUIRE(app.output.find("the_applications_own") != std::string::npos);
    REQUIRE(app.output.find("the_librarys_own") == std::string::npos);

    // and pointing at the library is how its own are run
    const ProcessResult lib = EchoTests::run_process({
        ECHOC_BINARY, "test", "-m", (project.root() / "lib").string() });

    INFO(lib.output);
    REQUIRE(lib.exit_code == 0);
    REQUIRE(lib.output.find("the_librarys_own") != std::string::npos);
}

// **the two halves of "a normal build never parses a test body", and the boundary between them.**
//
// the corpus asserts the first half: tests_eco/tests/a_normal_run_never_parses_one holds a body with a parse
// error and an unresolvable call, and its `mode: run` golden is the program's own output. What the corpus
// cannot assert is the second half, because a lexer error is an uncaught exception by design
// (ECO_DONT_CATCH_EXCEPTIONS) and the message on the way out is the C++ runtime's rather than echoc's -
// `libc++abi:` here and something else entirely on libstdc++. So the exit code is what is asserted, and the
// message deliberately is not
TEST_CASE("a test body is never parsed by a build, and is still lexed", "[test_targets]")
{
    ScopedProject project("dropped_bodies");

    // a parse error and two unresolvable names. **Compiles, and the program runs**: the tokens are dropped
    // between lexing and pass 1, so nothing ever reaches them
    write_file(project.root() / "unparseable.eco",
        "test never_compiled_here\n"
        "{\n"
        "    $a = no_such_function(no_such_argument);\n"
        "    struct Fixture { NoSuchType $x; }\n"
        "    $b = ;\n"
        "}\n"
        "echo(\"THE PROGRAM RAN\");\n");

    const ProcessResult ran = project.echoc("run unparseable.eco");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("THE PROGRAM RAN") != std::string::npos);

    // and the same body under `test` reports every one of them, which is what makes the assertion above an
    // assertion rather than a body that happened to be fine
    const ProcessResult tested = project.echoc("test unparseable.eco");
    INFO(tested.output);

    REQUIRE(tested.exit_code != 0);
    REQUIRE(tested.output.find("no_such_function") != std::string::npos);
    REQUIRE(tested.output.find("could not be read as a single value") != std::string::npos);

    // **a character the lexer has no rule for fails every build.** Lexing happens before the filter, so a
    // dropped body still has to have produced tokens - the same price an `#[if:]` region for another platform
    // pays, and the reason to run your tests rather than trusting a green build.
    //
    // a lexer error is an uncaught exception by design (ECO_DONT_CATCH_EXCEPTIONS), so this is the one case in
    // the suite whose child dies on a signal - and `sh` announces that on the *test binary's* stderr as
    // `Abort trap: 6`. Expected, captured by nothing, and asserted on by nothing: what is asserted is the exit
    // code and that the program never ran
    write_file(project.root() / "unlexable.eco",
        "test holds_a_character_the_lexer_refuses\n"
        "{\n"
        "    $a = 1 @ 2;\n"
        "}\n"
        "echo(\"NEVER REACHED\");\n");

    const ProcessResult unlexable = project.echoc("run unlexable.eco");
    INFO(unlexable.output);

    REQUIRE(unlexable.exit_code != 0);
    REQUIRE(unlexable.output.find("NEVER REACHED") == std::string::npos);
}

// a test build and a normal build hold different bodies for the same module, so they must not share an
// object. The flag reaches the key through TargetFacts::cache_signature, per module rather than per build
TEST_CASE("a test build and a normal build are keyed apart", "[test_targets]")
{
    ScopedProject project("keys_differ");
    write_grouped_project(project);

    const ProcessResult built = project.echoc("build --explain cache");
    const ProcessResult tested = project.echoc("test --explain cache");

    INFO("build: " << built.output << "\ntest: " << tested.output);

    REQUIRE(built.exit_code == 0);
    REQUIRE(tested.exit_code == 0);

    // the report names each module's key, so two runs that agreed about it would print the same hex
    REQUIRE(built.output != tested.output);
}
