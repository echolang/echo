#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "subprocess.h"

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
        EchoTests::echoc_binary(), "test", "-m", (project.root() / "app").string() });

    INFO(app.output);
    REQUIRE(app.exit_code == 0);
    REQUIRE(app.output.find("the_applications_own") != std::string::npos);
    REQUIRE(app.output.find("the_librarys_own") == std::string::npos);

    // and pointing at the library is how its own are run
    const ProcessResult lib = EchoTests::run_process({
        EchoTests::echoc_binary(), "test", "-m", (project.root() / "lib").string() });

    INFO(lib.output);
    REQUIRE(lib.exit_code == 0);
    REQUIRE(lib.output.find("the_librarys_own") != std::string::npos);
}

// **the two halves of "a normal build never parses a test body", and the boundary between them.**
//
// the corpus asserts the first half: tests_eco/tests/a_normal_run_never_parses_one holds a body with a parse
// error and an unresolvable call, and its `mode: run` golden is the program's own output. What the corpus
// cannot assert is the second half from a corpus golden: a lexer error is a located
// diagnostic at the character, caught whatever ECO_DONT_CATCH_EXCEPTIONS says. the exit code
// and the span are what this case asserts
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
    // a character the lexer has no rule for is a user error, reported as Tokenization Failed rather
    // than an uncaught exception. the program never ran
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
    REQUIRE(unlexable.output.find("unexpected '@'") != std::string::npos);
    REQUIRE(unlexable.output.find("unlexable.eco:") != std::string::npos);
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

TEST_CASE("a report names the abort site", "[test_targets][report]")
{
    ScopedProject project("report_abort_site");
    write_file(project.root() / "gives_up.eco",
        "test gives_up\n"
        "{\n"
        "    die('nothing to do here');\n"
        "}\n");

    const ProcessResult ran = project.echoc("test --report results.jsonl gives_up.eco");
    INFO(ran.output);

    REQUIRE(ran.exit_code != 0);

    std::ifstream in(project.root() / "results.jsonl");
    REQUIRE(in);
    const std::string jsonl((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE(jsonl.find("\"v\":2") != std::string::npos);
    REQUIRE(jsonl.find("\"headline\":\"fatal error\"") != std::string::npos);
    REQUIRE(jsonl.find("\"message\":\"nothing to do here\"") != std::string::npos);
    REQUIRE(jsonl.find("\"file\":\"gives_up.eco\"") != std::string::npos);
    REQUIRE(jsonl.find("\"line\":3") != std::string::npos);
}

namespace
{

// four modules, three of them tested together. `kit` owns a route table collected with `attr::of`
// and every other module registers into it:
//
//   one   -> kit            tests keep a `Rig`, a `KEY` and a "/shared" route at file scope
//   two   -> zed, kit       tests keep the same three names
//   three -> one            no test helpers
void write_route_modules(const ScopedProject &project)
{
    const auto manifest = [&project](
        const std::string &name, const std::string &depends, bool test_sources) {
        write_file(project.root() / name / "module.eco",
            "#[module: \"" + name + "\"]\n"
            "#[sources: \"src/*.eco\"]\n"
            + depends
            + (test_sources ? "#[target: test] {\n    #[sources: \"tests/*.eco\"]\n}\n" : ""));
    };

    const auto route = [](const std::string &name) {
        return "namespace " + name + ";\n"
            "\n"
            "#[http::get: \"/" + name + "\"]\n"
            "public function route() : int32\n"
            "{\n"
            "    return 1;\n"
            "}\n";
    };

    manifest("kit", "", false);
    write_file(project.root() / "kit/src/kit.eco",
        route("kit") +
        "\n"
        "public struct Route\n"
        "{\n"
        "    public string $path;\n"
        "    public function<int32()> $call;\n"
        "}\n"
        "\n"
        "public function paths() : string\n"
        "{\n"
        "    string $all = '';\n"
        "    foreach (attr::of<Route>(\"http::get\", \"path\") as const &$row) {\n"
        "        string $path = $row->path;\n"
        "        $all = \"{$all}{$path} \";\n"
        "    }\n"
        "    return $all;\n"
        "}\n");

    manifest("zed", "", false);
    write_file(project.root() / "zed/src/zed.eco", route("zed"));

    const auto helpers = [](const std::string &key) {
        return "internal const KEY = '" + key + "';\n"
            "\n"
            "struct Rig\n"
            "{\n"
            "    public string $key = KEY;\n"
            "}\n"
            "\n"
            "#[http::get: \"/shared\"]\n"
            "function shared_route() : int32\n"
            "{\n"
            "    return 1;\n"
            "}\n";
    };

    // a helper at file scope, then a namespace: the test below is no longer under the namespace
    // its own file's helper went into
    const auto suite = [](const std::string &name, const std::string &key, const std::string &paths) {
        return "function rig_key() : string\n"
            "{\n"
            "    Rig $rig = Rig();\n"
            "    return $rig->key;\n"
            "}\n"
            "\n"
            "namespace " + name + "::checks;\n"
            "\n"
            "test sees_its_own_program\n"
            "{\n"
            "    assert(rig_key() == '" + key + "');\n"
            "    assert(kit::paths() == '" + paths + "');\n"
            "}\n";
    };

    manifest("one", "#[depends: \"../kit\"]\n", true);
    write_file(project.root() / "one/src/one.eco", route("one"));
    write_file(project.root() / "one/tests/helpers.eco", helpers("of one"));
    write_file(project.root() / "one/tests/suite.eco", suite("one", "of one", "/kit /one /shared "));

    // `zed` before `kit`, which is not the order the run as a whole meets them in
    manifest("two", "#[depends: \"../zed\"]\n#[depends: \"../kit\"]\n", true);
    write_file(project.root() / "two/src/two.eco", route("two"));
    write_file(project.root() / "two/tests/helpers.eco", helpers("of two"));
    write_file(project.root() / "two/tests/suite.eco", suite("two", "of two", "/zed /kit /two /shared "));

    manifest("three", "#[depends: \"../one\"]\n#[depends: \"../kit\"]\n", false);
    write_file(project.root() / "three/src/three.eco",
        route("three") +
        "\n"
        "test sees_its_own_program\n"
        "{\n"
        "    assert(kit::paths() == '/kit /one /three ');\n"
        "}\n");
}

};

// **one program, and still each root's own.** three things a root's own `echoc test` gives it for free
// and one shared build has to give back: the names its test files keep at file scope are its own, a
// table collected from "the program" holds its program and not its neighbours', and in the order its
// own build would have met the modules
TEST_CASE("each root of a test run over several keeps its own program", "[test_targets]")
{
    ScopedProject project("several_programs");
    write_route_modules(project);

    const std::string cache = (project.root() / "cache").string();
    const auto root = [&project](const std::string &name) {
        return (project.root() / name).string();
    };

    // what each asserts is what it sees alone, so the same suites have to pass both ways
    for (const std::string &name : { "one", "two", "three" }) {
        const ProcessResult alone = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", root(name), "--build-dir", cache });

        INFO(alone.output);
        REQUIRE(alone.exit_code == 0);
        REQUIRE(alone.output.find("1 test passed") != std::string::npos);
    }

    const ProcessResult together = EchoTests::run_process({
        EchoTests::echoc_binary(), "test", "-m", root("one"), "-m", root("two"), "-m", root("three"),
        "--build-dir", cache });

    INFO(together.output);
    REQUIRE(together.exit_code == 0);
    REQUIRE(together.output.find("3 tests passed") != std::string::npos);

    // `zed` is in the bundle because `two` asked for it, and public: nothing but its own manifest
    // says `three` may not name it - which is the one thing a build of `three` alone goes by
    SECTION("a root naming a module it does not depend on is refused")
    {
        write_file(project.root() / "three/src/reach.eco",
            "namespace three;\n"
            "\n"
            "public function reaches_over() : int32\n"
            "{\n"
            "    return zed::route();\n"
            "}\n");

        const ProcessResult refused = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", root("one"), "-m", root("two"), "-m", root("three"),
            "--build-dir", cache });

        INFO(refused.output);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find(
            "is declared in the module 'zed', which the module 'three' does not depend on")
            != std::string::npos);

        // and the same source is as wrong alone, where there is no `zed` at all
        const ProcessResult alone = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", root("three"), "--build-dir", cache });

        INFO(alone.output);
        REQUIRE(alone.exit_code != 0);
    }

    // each root's `str::from(const Rig&)` takes its own private `Rig`, so the two are two overloads
    SECTION("two roots each extend str for their own file-scope type")
    {
        for (const std::string &name : { "one", "two" }) {
            write_file(project.root() / name / "tests/str.eco",
                "namespace str;\n"
                "\n"
                "function from(const Rig& $r) : string\n"
                "{\n"
                "    return $r->key;\n"
                "}\n");
            write_file(project.root() / name / "tests/text.eco",
                "test a_rig_renders_as_its_key\n"
                "{\n"
                "    Rig $r = Rig();\n"
                "    assert(\"{$r}\" == KEY);\n"
                "}\n");
        }

        const ProcessResult extended = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", root("one"), "-m", root("two"), "-m", root("three"),
            "--build-dir", cache });

        INFO(extended.output);
        REQUIRE(extended.exit_code == 0);
        REQUIRE(extended.output.find("5 tests passed") != std::string::npos);
    }

    // `three` depends on `one`, and still has none of `one`'s tests when it builds alone
    SECTION("what one root's test files declare is not another root's")
    {
        write_file(project.root() / "one/tests/probe.eco",
            "namespace one::probe;\n"
            "\n"
            "public function answer() : int32\n"
            "{\n"
            "    return 42;\n"
            "}\n");
        write_file(project.root() / "three/src/probe.eco",
            "test reaches_into_the_tests_of_one\n"
            "{\n"
            "    assert(one::probe::answer() == 42);\n"
            "}\n");

        const ProcessResult refused = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", root("one"), "-m", root("two"), "-m", root("three"),
            "--build-dir", cache });

        INFO(refused.output);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find(
            "is declared in a test file of the module 'one', and only that module's own tests can name it")
            != std::string::npos);
    }

    SECTION("a name one root's own program registers twice is still refused")
    {
        write_file(project.root() / "two/tests/again.eco",
            "#[http::get: \"/two\"]\n"
            "function two_again() : int32\n"
            "{\n"
            "    return 1;\n"
            "}\n");

        const ProcessResult refused = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", root("one"), "-m", root("two"), "-m", root("three"),
            "--build-dir", cache });

        INFO(refused.output);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find("'/two' is used twice as the 'path' of 'http::get'") != std::string::npos);
        REQUIRE(refused.output.find("'/shared' is used twice") == std::string::npos);
    }
}

// **a workspace is the `-m` list written down once.** the manifest names its members and
// `--workspace` is the run that names them all, so the list a person types, the one CI types and the
// one a package manager installs for cannot be three lists
TEST_CASE("a workspace run tests the module and every member it lists", "[test_targets][workspace]")
{
    ScopedProject project("workspace");
    write_route_modules(project);

    // `one`, `two` and `three` beside a root that is a member by being where the list is
    write_file(project.root() / "module.eco",
        "#[module: \"root\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[workspace: [\"t*\", \"one\"]]\n");

    write_file(project.root() / "src/root.eco",
        "namespace root;\n"
        "\n"
        "test is_a_member_too\n"
        "{\n"
        "    assert(true);\n"
        "}\n");

    const ProcessResult ran = project.echoc("test --workspace");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("4 tests passed") != std::string::npos);
    REQUIRE(ran.output.find("root/root.eco::is_a_member_too") != std::string::npos);

    SECTION("the manifest dump names the members a pattern resolved to")
    {
        const ProcessResult dumped = project.echoc("build -p manifest");
        INFO(dumped.output);

        REQUIRE(dumped.exit_code == 0);
        REQUIRE(dumped.output.find("\"workspace\": [\"t*\", \"one\"]") != std::string::npos);
        REQUIRE(dumped.output.find("\"members\": [\"three\", \"two\", \"one\"]") != std::string::npos);
    }

    SECTION("a pattern that matches no module is refused")
    {
        write_file(project.root() / "module.eco",
            "#[module: \"root\"]\n"
            "#[sources: \"src/*.eco\"]\n"
            "#[workspace: [\"one\", \"gone/*\"]]\n");

        const ProcessResult refused = project.echoc("test --workspace");
        INFO(refused.output);

        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find("No Such Member") != std::string::npos);
        REQUIRE(refused.output.find("'gone/*'") != std::string::npos);
    }

    SECTION("a module that lists no members has no workspace to run")
    {
        const ProcessResult refused = project.echoc(
            "test --workspace -m " + EchoTests::quoted(project.root() / "one"));
        INFO(refused.output);

        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find("No Workspace") != std::string::npos);
    }

    SECTION("without the flag the members are not this run's")
    {
        const ProcessResult alone = project.echoc("test");
        INFO(alone.output);

        REQUIRE(alone.exit_code == 0);
        REQUIRE(alone.output.find("1 test passed") != std::string::npos);
    }
}

// **a workspace is installed once, at its root.** a member has no `vendor/` of its own then, and is
// still built from its own directory - by a person, and by any hook tool that asks echoc about the
// module it was run for
TEST_CASE("a workspace member finds its packages at the workspace root", "[test_targets][workspace]")
{
    ScopedProject project("workspace_packages");

    write_file(project.root() / "module.eco",
        "#[module: \"root\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[workspace: [\"parts/*\"]]\n");
    write_file(project.root() / "src/root.eco", "namespace root;\n");

    write_file(project.root() / "vendor/libhello/module.eco",
        "#[module: \"libhello\"]\n"
        "#[version: \"1.0.0\"]\n"
        "#[sources: \"src/*.eco\"]\n");
    write_file(project.root() / "vendor/libhello/src/hello.eco",
        "namespace libhello;\n"
        "public function answer() : int32 { return 42; }\n");

    const auto part = [&project](const std::string &name) {
        write_file(project.root() / "parts" / name / "module.eco",
            "#[module: \"" + name + "\"]\n"
            "#[requires: \"libhello\" { version: \"^1.0\", source: git \"https://example.com/libhello\" }]\n"
            "#[sources: \"src/*.eco\"]\n");
        write_file(project.root() / "parts" / name / "src/suite.eco",
            "namespace " + name + ";\n"
            "\n"
            "test asks_the_package\n"
            "{\n"
            "    assert(libhello::answer() == 42);\n"
            "}\n");
    };

    part("inside");

    const ProcessResult member = EchoTests::run_process({
        EchoTests::echoc_binary(), "test", "-m", (project.root() / "parts/inside").string() });
    INFO(member.output);
    REQUIRE(member.exit_code == 0);
    REQUIRE(member.output.find("1 test passed") != std::string::npos);

    const ProcessResult all = project.echoc("test --workspace");
    INFO(all.output);
    REQUIRE(all.exit_code == 0);
    REQUIRE(all.output.find("1 test passed") != std::string::npos);

    SECTION("a module the workspace does not list is not its member")
    {
        write_file(project.root() / "other/outside/module.eco",
            "#[module: \"outside\"]\n"
            "#[requires: \"libhello\" { version: \"^1.0\", source: git \"https://example.com/libhello\" }]\n"
            "#[sources: \"src/*.eco\"]\n");
        write_file(project.root() / "other/outside/src/lib.eco", "namespace outside;\n");

        const ProcessResult refused = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", (project.root() / "other/outside").string() });
        INFO(refused.output);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find("is not in 'vendor/'") != std::string::npos);
    }
}

// **a test run pointed at several modules is one build and one runner.** none of them is "the program", so
// the question several roots cannot answer for `build` is not asked: each contributes its tests
TEST_CASE("a test run over several modules runs the tests of each", "[test_targets]")
{
    ScopedProject project("several_modules");

    // the same file name, the same test name, at the same line and column: only the module tells the two
    // apart, in the runner's symbol as much as in what is printed
    for (const std::string &name : { "one", "two" }) {
        write_file(project.root() / name / "module.eco",
            "#[module: \"" + name + "\"]\n"
            "#[sources: \"src/*.eco\"]\n");

        write_file(project.root() / name / "src/suite.eco",
            "namespace " + name + ";\n"
            "\n"
            "test adds_up\n"
            "{\n"
            "    assert(1 + 1 == 2);\n"
            "}\n");
    }

    const std::string one = (project.root() / "one").string();
    const std::string two = (project.root() / "two").string();
    const std::string cache = (project.root() / "cache").string();

    const ProcessResult both = EchoTests::run_process({
        EchoTests::echoc_binary(), "test", "-m", one, "-m", two, "--build-dir", cache });

    INFO(both.output);
    REQUIRE(both.exit_code == 0);
    REQUIRE(both.output.find("one/suite.eco::adds_up") != std::string::npos);
    REQUIRE(both.output.find("two/suite.eco::adds_up") != std::string::npos);
    REQUIRE(both.output.find("2 tests passed") != std::string::npos);

    SECTION("a module filter selects one of them")
    {
        const ProcessResult narrowed = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", one, "-m", two, "--build-dir", cache,
            "--filter", "module:two" });

        INFO(narrowed.output);
        REQUIRE(narrowed.exit_code == 0);
        REQUIRE(narrowed.output.find("two/suite.eco::adds_up") != std::string::npos);
        REQUIRE(narrowed.output.find("one/suite.eco::adds_up") == std::string::npos);
    }

    SECTION("a second run builds none of them again")
    {
        const ProcessResult again = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", one, "-m", two, "--build-dir", cache,
            "--explain", "cache" });

        INFO(again.output);
        REQUIRE(again.exit_code == 0);
        REQUIRE(EchoTests::line_starting_with(again.output, "one").find("hit") != std::string::npos);
        REQUIRE(EchoTests::line_starting_with(again.output, "two").find("hit") != std::string::npos);
    }

    SECTION("a target is one module's, so it is refused")
    {
        const ProcessResult refused = EchoTests::run_process({
            EchoTests::echoc_binary(), "test", "-m", one, "-m", two, "--build-dir", cache,
            "--target", "quick" });

        INFO(refused.output);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find("No Such Target") != std::string::npos);
        REQUIRE(refused.output.find("several") != std::string::npos);
    }

    SECTION("every other command still wants one program")
    {
        const ProcessResult refused = EchoTests::run_process({
            EchoTests::echoc_binary(), "build", "-m", one, "-m", two, "--build-dir", cache });

        INFO(refused.output);
        REQUIRE(refused.exit_code != 0);
        REQUIRE(refused.output.find("Several manifests were given") != std::string::npos);
    }
}

// **a namespace a test file writes is the program's.** only what a test scope adds at *file scope* is the
// module's tests' own; `namespace str;`, the module's own namespace and a generator's namespace reopen the
// one every other file sees, and a lookup from inside any of them still finds the file-scope helpers
TEST_CASE("a test file reopens the program's namespaces and keeps its file-scope helpers", "[test_targets]")
{
    ScopedProject project("reopens_namespaces");

    write_file(project.root() / "module.eco",
        "#[module: \"shelf\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: test] {\n"
        "    #[sources: \"tests/*.eco\"]\n"
        "    #[sources: \"gen/*.eco\"]\n"
        "}\n");

    write_file(project.root() / "src/shelf.eco",
        "namespace shelf;\n"
        "\n"
        "internal function weight() : int32\n"
        "{\n"
        "    return 7;\n"
        "}\n"
        "\n"
        "public function weigh(int32 $n) : int32\n"
        "{\n"
        "    return $n;\n"
        "}\n"
        "\n"
        "public function total<T>(const array<T>& $items) : int32\n"
        "{\n"
        "    int32 $sum = 0;\n"
        "    foreach ($items as const &$item) {\n"
        "        $sum = $sum + weigh($item);\n"
        "    }\n"
        "    return $sum;\n"
        "}\n");

    write_file(project.root() / "src/root.eco",
        "public function describe(int32 $n) : string\n"
        "{\n"
        "    return 'number';\n"
        "}\n");

    // file scope: the module's tests' own
    write_file(project.root() / "tests/helpers.eco",
        "struct Rig\n"
        "{\n"
        "    public int32 $n;\n"
        "}\n"
        "\n"
        "internal const RIG_NAME = 'rig';\n"
        "\n"
        "function make_rig(int32 $n) : Rig\n"
        "{\n"
        "    return Rig($n);\n"
        "}\n"
        "\n"
        "// hides the root's `describe(int32)` from the tests, as any inner namespace would\n"
        "function describe(const Rig& $r) : string\n"
        "{\n"
        "    return RIG_NAME;\n"
        "}\n");

    write_file(project.root() / "tests/str.eco",
        "namespace str;\n"
        "\n"
        "function from(const Rig& $r) : string\n"
        "{\n"
        "    return RIG_NAME;\n"
        "}\n");

    write_file(project.root() / "tests/weigh.eco",
        "namespace shelf;\n"
        "\n"
        "function weigh(const Rig& $r) : int32\n"
        "{\n"
        "    return $r->n;\n"
        "}\n");

    write_file(project.root() / "gen/reg.eco",
        "namespace gen::reg;\n"
        "\n"
        "function size() : int32\n"
        "{\n"
        "    return 2;\n"
        "}\n");

    write_file(project.root() / "tests/suite.eco",
        "namespace shelf::checks;\n"
        "\n"
        "use gen::reg;\n"
        "\n"
        "test sees_the_program_and_its_helpers\n"
        "{\n"
        "    Rig $r = make_rig(3);\n"
        "    assert(\"{$r}\" == 'rig');\n"
        "    assert(shelf::weight() == 7);\n"
        "    assert(shelf::total([make_rig(1), make_rig(2)]) == 3);\n"
        "    assert(reg::size() == 2);\n"
        "    assert(describe($r) == 'rig');\n"
        "}\n");

    const ProcessResult ran = project.echoc("test");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("1 test passed") != std::string::npos);

    // a bare `namespace;` is back to file scope, which for a test file is still its own - and its own
    // `Spare` hides the root's, constructors included
    SECTION("a bare namespace returns to the file's own scope")
    {
        write_file(project.root() / "tests/back.eco",
            "namespace shelf;\n"
            "\n"
            "namespace;\n"
            "\n"
            "struct Spare\n"
            "{\n"
            "    public int32 $n = 1;\n"
            "}\n"
            "\n"
            "test the_spare_is_the_tests_own\n"
            "{\n"
            "    Spare $s = Spare();\n"
            "    assert($s->n == 1);\n"
            "}\n");
        write_file(project.root() / "src/spare.eco",
            "struct Spare\n"
            "{\n"
            "    public int32 $n = 2;\n"
            "}\n");

        const ProcessResult shadowed = project.echoc("test");
        INFO(shadowed.output);
        REQUIRE(shadowed.exit_code == 0);
        REQUIRE(shadowed.output.find("2 tests passed") != std::string::npos);
    }
}
