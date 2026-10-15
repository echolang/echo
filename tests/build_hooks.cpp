#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "Compiler/ModuleCache.h"
#include "Compiler/PublicTools.h"
#include "subprocess.h"

// a `#[hook: before_build]` runs before `#[sources:]` expand, onto stderr, and only the root's.
// subprocess: a corpus case is never a project with a root manifest of its own.

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("build_hooks", name)
    {};
};

// a library offering `gen`: writes `api_answer` at `<out>` only when the text would change
void write_generator(const std::filesystem::path &dir)
{
    write_file(dir / "module.eco",
        "#[module: \"restgen\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"gen\", entry: \"cli/main.eco\", public: true }] {\n"
        "    #[sources: \"cli/*.eco\"]\n"
        "}\n");

    write_file(dir / "src/lib.eco", "namespace restgen;\npublic function version() : int32 { return 1; }\n");

    write_file(dir / "cli/main.eco",
        "array<string> $argv = std::env::argv();\n"
        "string $spec = std::io::readfile($argv[1])->or('');\n"
        "string $out = $argv[2];\n"
        "string $want = \"public function api_answer() : int32 { return {$spec}; }\\n\";\n"
        "string $have = std::io::readfile($out)->or('');\n"
        "string $state = string::from(std::env::var('ECHO_HOOK_STATE', ''));\n"
        "string $seen = \"cwd-relative-spec={$argv[1]}\\n\";\n"
        "$seen->append(\"module={$state}\\n\");\n"
        "$seen->append(\"command=\");\n"
        "$seen->append(string::from(std::env::var('ECHO_COMMAND', '')));\n"
        "$seen->append(\"\\nos=\");\n"
        "$seen->append(string::from(std::env::var('ECHO_TARGET_OS', '')));\n"
        "$seen->append(\"\\nmodule_dir=\");\n"
        "$seen->append(string::from(std::env::var('ECHO_MODULE_DIR', '')));\n"
        "$seen->append(\"\\n\");\n"
        "std::io::writefile(\"{$state}/seen\", $seen);\n"
        "if ($have == $want) {\n"
        "    echo 'GENERATOR SAYS UP TO DATE';\n"
        "} else {\n"
        "    std::io::writefile($out, $want);\n"
        "    echo 'GENERATOR WROTE';\n"
        "}\n");
}

// an app whose `api_answer` comes from `gen/`, empty until the hook runs
void write_app(const std::filesystem::path &dir, const std::string &hook_scope)
{
    write_file(dir / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[sources: \"gen/*.eco\"]\n"
        "#[hook: before_build { run: \"gen\", args: [\"api/spec.txt\", \"gen/api.eco\"] }]" + hook_scope + "\n");

    write_file(dir / "api/spec.txt", "7");
    write_file(dir / "src/main.eco", "echo api_answer() * 6;\n");

    std::error_code ec;
    std::filesystem::create_directories(dir / "gen", ec);
}

};

TEST_CASE("a hook writes what the build then compiles", "[hooks]")
{
    ScopedProject project("writes_what_is_compiled");

    write_generator(project.root() / "restgen");
    write_app(project.root() / "app", " {\n    #[depends: \"../restgen\"]\n}");

    const std::filesystem::path app = project.root() / "app";

    // `gen/` is empty: only the hook makes `gen/*.eco` match anything at all
    const ProcessResult first = project.echoc("run", app);
    INFO(first.output);

    REQUIRE(first.exit_code == 0);
    REQUIRE(first.output.find("GENERATOR WROTE") != std::string::npos);
    REQUIRE(first.output.find("42") != std::string::npos);

    SECTION("it runs on every build, and its tool decides there is nothing to do")
    {
        const std::filesystem::path tool = EchoTests::with_exe(project.root() / "restgen/ecobuild/gen");
        std::error_code ec;
        const auto built = std::filesystem::last_write_time(tool, ec);

        const ProcessResult second = project.echoc("run", app);
        INFO(second.output);

        REQUIRE(second.exit_code == 0);
        REQUIRE(second.output.find("GENERATOR SAYS UP TO DATE") != std::string::npos);
        REQUIRE(second.output.find("42") != std::string::npos);

        // the tool's program stamp answered for the child build: the tool itself was not linked again
        const bool tool_untouched = std::filesystem::last_write_time(tool, ec) == built;
        REQUIRE(tool_untouched);
    }

    SECTION("an input the tool reads changes what is compiled")
    {
        write_file(app / "api/spec.txt", "8");

        const ProcessResult changed = project.echoc("run", app);
        INFO(changed.output);

        REQUIRE(changed.output.find("GENERATOR WROTE") != std::string::npos);
        REQUIRE(changed.output.find("48") != std::string::npos);
    }

    SECTION("an edited tool is built again before it runs")
    {
        write_file(project.root() / "restgen/cli/extra.eco", "function unused_by_anyone() : void {}\n");

        const ProcessResult rebuilt = project.echoc("run", app);
        INFO(rebuilt.output);

        REQUIRE(rebuilt.exit_code == 0);
        REQUIRE(rebuilt.output.find("42") != std::string::npos);
    }

    SECTION("it runs in the module's directory, with the protocol's environment")
    {
        const std::string seen = Compiler::read_whole_file(app / "ecobuild/hooks/gen/seen").value_or("");
        INFO(seen);

        // a relative argument meant something, so the working directory was the module's
        REQUIRE(seen.find("cwd-relative-spec=api/spec.txt") != std::string::npos);
        REQUIRE(seen.find("command=run") != std::string::npos);
        REQUIRE(seen.find("os=") != std::string::npos);
        REQUIRE(seen.find("module_dir=") != std::string::npos);
        REQUIRE(EchoTests::output_contains_path(seen, app / "ecobuild" / "hooks" / "gen"));
    }

    SECTION("--no-hooks compiles what is on disk, stale or not")
    {
        write_file(app / "api/spec.txt", "9");

        const ProcessResult skipped = project.echoc("run --no-hooks", app);
        INFO(skipped.output);

        REQUIRE(skipped.exit_code == 0);
        REQUIRE(skipped.output.find("GENERATOR") == std::string::npos);
        REQUIRE(skipped.output.find("42") != std::string::npos);
    }

    SECTION("a tool-only dependency is never part of the program")
    {
        const ProcessResult explained = project.echoc("build -o out --explain cache", app);
        INFO(explained.output);

        REQUIRE(explained.exit_code == 0);
        REQUIRE(EchoTests::line_starting_with(explained.output, "restgen").empty());
    }
}

TEST_CASE("a hook's output is never the program's stdout", "[hooks]")
{
    ScopedProject project("not_on_stdout");

    write_generator(project.root() / "restgen");
    write_app(project.root() / "app", " {\n    #[depends: \"../restgen\"]\n}");

#if defined(_WIN32)
    const char *quiet_stderr = " 2>nul";
#else
    const char *quiet_stderr = " 2>/dev/null";
#endif

    const ProcessResult stdout_only = EchoTests::run_capturing(
        "cd " + EchoTests::quoted(project.root() / "app") + " && "
        + EchoTests::quoted(EchoTests::echoc_binary()) + " run" + quiet_stderr);
    INFO(stdout_only.output);

    REQUIRE(stdout_only.exit_code == 0);
    REQUIRE(stdout_only.output.find("42") != std::string::npos);
    REQUIRE(stdout_only.output.find("GENERATOR") == std::string::npos);
}

TEST_CASE("a failing hook fails the build, at its line", "[hooks]")
{
    ScopedProject project("failing");

    write_file(project.root() / "broken/module.eco",
        "#[module: \"broken\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"fail\", entry: \"src/main.eco\", public: true }]\n");
    write_file(project.root() / "broken/src/main.eco", "echo 'THE TOOL COMPLAINED';\nstd::env::exit(3);\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../broken\"]\n"
        "#[hook: before_build { run: \"fail\" }]\n");
    write_file(project.root() / "app/src/main.eco", "echo 'THE PROGRAM RAN';\n");

    SECTION("streamed")
    {
        const ProcessResult failed = project.echoc("run", project.root() / "app");
        INFO(failed.output);

        REQUIRE(failed.exit_code == 1);
        REQUIRE(failed.output.find("THE TOOL COMPLAINED") != std::string::npos);
        REQUIRE(failed.output.find("hook 'fail' failed") != std::string::npos);
        REQUIRE(failed.output.find("exited with 3") != std::string::npos);
        REQUIRE(failed.output.find("module.eco:4") != std::string::npos);
        REQUIRE(failed.output.find("THE PROGRAM RAN") == std::string::npos);
    }

    SECTION("under json, the tool's output travels inside the diagnostic")
    {
        const ProcessResult failed = project.echoc("run --diagnostics=json", project.root() / "app");
        INFO(failed.output);

        REQUIRE(failed.exit_code == 1);

        // every line is JSON: the tool's chatter is a note, never a bare line between two diagnostics
        std::istringstream lines(failed.output);
        std::string line;
        bool found = false;

        while (std::getline(lines, line)) {
            if (line.empty()) {
                continue;
            }

            REQUIRE(line.front() == '{');

            if (line.find("THE TOOL COMPLAINED") != std::string::npos) {
                found = true;
            }
        }

        REQUIRE(found);
    }
}

TEST_CASE("only the root runs its hooks", "[hooks]")
{
    // a library whose own hook would fail: a consumer building against it must never see it
    ScopedProject project("root_only");

    write_file(project.root() / "broken/module.eco",
        "#[module: \"broken\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"fail\", entry: \"cli/main.eco\", public: true }] {\n"
        "    #[sources: \"cli/*.eco\"]\n"
        "}\n");
    write_file(project.root() / "broken/src/api.eco", "namespace broken;\npublic function answer() : int32 { return 42; }\n");
    write_file(project.root() / "broken/cli/main.eco", "echo 'THE TOOL RAN';\nstd::env::exit(3);\n");

    write_file(project.root() / "lib/module.eco",
        "#[module: \"lib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../broken\"]\n"
        "#[hook: before_build { run: \"fail\" }]\n");
    write_file(project.root() / "lib/src/lib.eco", "namespace lib;\npublic function answer() : int32 { return broken::answer(); }\n");

    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../lib\"]\n");
    write_file(project.root() / "app/src/main.eco", "echo lib::answer();\n");

    const ProcessResult ran = project.echoc("run", project.root() / "app");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("42") != std::string::npos);
    REQUIRE(ran.output.find("THE TOOL RAN") == std::string::npos);

    // and building the library itself is when its author's hook does run
    const ProcessResult own = project.echoc("build -o out", project.root() / "lib");
    REQUIRE(own.exit_code == 1);
    REQUIRE(own.output.find("THE TOOL RAN") != std::string::npos);
}

TEST_CASE("asking what a project is runs none of its hooks", "[hooks]")
{
    ScopedProject project("questions_run_nothing");

    write_generator(project.root() / "restgen");
    write_app(project.root() / "app", " {\n    #[depends: \"../restgen\"]\n}");

    const std::filesystem::path app = project.root() / "app";

    REQUIRE(project.echoc("build -p manifest", app).exit_code == 0);
    REQUIRE(project.echoc("build -p tools", app).exit_code == 0);
    REQUIRE(project.echoc("clean", app).exit_code == 0);

    REQUIRE_FALSE(EchoTests::file_exists(app / "gen/api.eco"));
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "restgen/ecobuild/gen"));
}

TEST_CASE("a hook's tool must be one a direct dependency offers", "[hooks]")
{
    ScopedProject project("offered_tools_only");

    write_generator(project.root() / "restgen");

    SECTION("a tool nobody offers is refused at the hook's line")
    {
        write_file(project.root() / "app/module.eco",
            "#[module: \"app\"]\n"
            "#[sources: \"src/*.eco\"]\n"
            "#[depends: \"../restgen\"]\n"
            "#[hook: before_build { run: \"nope\" }]\n");
        write_file(project.root() / "app/src/main.eco", "echo 1;\n");

        const ProcessResult refused = project.echoc("run", project.root() / "app");
        INFO(refused.output);

        REQUIRE(refused.exit_code == 1);
        REQUIRE(refused.output.find("no dependency of 'app' offers a tool called 'nope'") != std::string::npos);
        REQUIRE(refused.output.find("The tools on offer are: gen.") != std::string::npos);
        REQUIRE(refused.output.find("module.eco:4") != std::string::npos);
    }

    SECTION("an unvendored tool package is refused when the hook needs it")
    {
        write_file(project.root() / "app/module.eco",
            "#[module: \"app\"]\n"
            "#[sources: \"src/*.eco\"]\n"
            "#[hook: before_build { run: \"gen\" }] {\n"
            "    #[requires: \"restgen\" { version: \"^1\", source: git \"https://example.com/restgen\" }]\n"
            "}\n");
        write_file(project.root() / "app/src/main.eco", "echo 1;\n");

        const ProcessResult refused = project.echoc("run", project.root() / "app");
        INFO(refused.output);

        REQUIRE(refused.exit_code == 1);
        REQUIRE(refused.output.find("the package \"restgen\" is not in 'vendor/', and hook 'gen' requires it")
            != std::string::npos);
        REQUIRE(refused.output.find("epm install") != std::string::npos);

        // the same project builds with its hooks off: nothing else about it needs the package
        REQUIRE(project.echoc("run --no-hooks", project.root() / "app").exit_code == 0);
    }
}

TEST_CASE("an unusable hook state directory refuses the build", "[hooks]")
{
    ScopedProject project("unusable_state");

    write_generator(project.root() / "restgen");
    write_app(project.root() / "app", " {\n    #[depends: \"../restgen\"]\n}");

    const std::filesystem::path app = project.root() / "app";
    const std::filesystem::path blocked = app / "ecobuild" / "hooks" / "gen";

    std::error_code ec;
    std::filesystem::create_directories(blocked.parent_path(), ec);
    write_file(blocked, "not a directory\n");

    const ProcessResult refused = project.echoc("run", app);
    INFO(refused.output);

    REQUIRE(refused.exit_code == 1);
    REQUIRE(refused.output.find("Cannot Build Here") != std::string::npos);
    REQUIRE(EchoTests::output_contains_path(refused.output, blocked));
    REQUIRE(refused.output.find("42") == std::string::npos);
}

TEST_CASE("several roots with a hook are refused", "[hooks]")
{
    ScopedProject project("several_roots");

    write_generator(project.root() / "restgen");
    write_app(project.root() / "app", " {\n    #[depends: \"../restgen\"]\n}");

    write_file(project.root() / "other/module.eco",
        "#[module: \"other\"]\n"
        "#[sources: \"src/*.eco\"]\n");
    write_file(project.root() / "other/src/lib.eco", "namespace other;\n");

    const ProcessResult refused = project.echoc(
        "run -m " + EchoTests::quoted(project.root() / "app")
        + " -m " + EchoTests::quoted(project.root() / "other"));
    INFO(refused.output);

    REQUIRE(refused.exit_code == 1);
    REQUIRE(refused.output.find("Several Roots") != std::string::npos);
    REQUIRE(refused.output.find("name one '-m'") != std::string::npos);
}

TEST_CASE("select_public_tool names what there was to choose from", "[hooks][public_tools]")
{
    Compiler::PublicToolSet set;
    set.consumer = "app";
    set.dependencies = { "one", "two" };
    set.own_targets = { "app:server" };
    set.unoffered = { "one:internal" };
    set.tools = {
        Compiler::PublicTool{ "one", "gen", "/one/module.eco", "/one/ecobuild/gen", { "one:gen" } },
        Compiler::PublicTool{ "two", "gen", "/two/module.eco", "/two/ecobuild/gen", { "two:gen" } },
        Compiler::PublicTool{ "two", "fmt", "/two/module.eco", "/two/ecobuild/fmt", { "fmt", "two:fmt" } },
    };

    std::string refusal;

    REQUIRE(Compiler::select_public_tool(set, "fmt", refusal) != nullptr);
    REQUIRE(Compiler::select_public_tool(set, "two:fmt", refusal)->target == "fmt");
    REQUIRE(Compiler::select_public_tool(set, "one:gen", refusal)->module == "one");

    SECTION("a bare name two dependencies offer")
    {
        REQUIRE(Compiler::select_public_tool(set, "gen", refusal) == nullptr);
        REQUIRE(refusal.find("offered by more than one dependency") != std::string::npos);
        REQUIRE(refusal.find("one:gen, two:gen") != std::string::npos);
    }

    SECTION("a target its module does not offer")
    {
        REQUIRE(Compiler::select_public_tool(set, "internal", refusal) == nullptr);
        REQUIRE(refusal.find("'one:internal' is a target of a dependency but is not offered") != std::string::npos);
    }

    SECTION("the consumer's own target")
    {
        REQUIRE(Compiler::select_public_tool(set, "server", refusal) == nullptr);
        REQUIRE(refusal.find("one of 'app''s own targets") != std::string::npos);
        REQUIRE(refusal.find("echoc run --target server") != std::string::npos);
    }

    SECTION("a module it does not depend on directly")
    {
        REQUIRE(Compiler::select_public_tool(set, "three:gen", refusal) == nullptr);
        REQUIRE(refusal.find("'three' is not a module 'app' depends on directly") != std::string::npos);
    }
}
