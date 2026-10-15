#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Compiler/CodegenTarget.h"
#include "Compiler/ModuleCache.h"
#include "subprocess.h"

// a project: a loose file's scratch dies with the process

namespace
{
    std::filesystem::path runner_path(const EchoTests::ScopedProject &project)
    {
        return Compiler::CodegenTarget{}.output_path(
            project.root() / "ecobuild" / "scratch" / "eco_run");
    }

    std::filesystem::path runner_link_record(const EchoTests::ScopedProject &project)
    {
        std::filesystem::path record = runner_path(project);
        record += ".link";
        return record;
    }

    std::filesystem::file_time_type runner_time(const EchoTests::ScopedProject &project)
    {
        return std::filesystem::last_write_time(runner_path(project));
    }
}

TEST_CASE("an unchanged run keeps its runner, and an edit relinks it", "[cli][cache]")
{
    EchoTests::ScopedProject project("run_link_reuse", "edit");
    EchoTests::write_file(project.root() / "module.eco", "#[module: \"reuse\"]\n#[sources: \"*.eco\"]\n");
    EchoTests::write_file(project.root() / "main.eco", "echo 'one';\n");

    const EchoTests::ProcessResult first = project.echoc("run");
    REQUIRE(first.exit_code == 0);
    REQUIRE(first.output.find("one") != std::string::npos);
    REQUIRE(std::filesystem::exists(runner_link_record(project)));

    const auto linked = runner_time(project);

    const EchoTests::ProcessResult again = project.echoc("run");
    REQUIRE(again.exit_code == 0);
    REQUIRE(again.output.find("one") != std::string::npos);
    const bool kept = runner_time(project) == linked;
    REQUIRE(kept);

    EchoTests::write_file(project.root() / "main.eco", "echo 'two';\n");

    const EchoTests::ProcessResult edited = project.echoc("run");
    REQUIRE(edited.exit_code == 0);
    REQUIRE(edited.output.find("two") != std::string::npos);
}

TEST_CASE("a run under -g always relinks, its debug map naming objects by mtime", "[cli][cache]")
{
    EchoTests::ScopedProject project("run_link_reuse", "debug");
    EchoTests::write_file(project.root() / "module.eco", "#[module: \"reuse\"]\n#[sources: \"*.eco\"]\n");
    EchoTests::write_file(project.root() / "main.eco", "echo 'one';\n");

    REQUIRE(project.echoc("run -g").exit_code == 0);
    REQUIRE_FALSE(std::filesystem::exists(runner_link_record(project)));
}

TEST_CASE("a link digest keys on the object path, so a same-path rewrite agrees", "[cache][link]")
{
    EchoTests::ScopedProject project("run_link_reuse", "digest");
    const std::filesystem::path object = project.root() / "mod-abc.o";
    {
        std::ofstream(object, std::ios::binary) << "one";
    }

    const std::vector<std::string> command = { "ld", "-o", "a", object.string() };
    const auto first = Compiler::link_inputs_digest(command, { object });
    {
        std::ofstream(object, std::ios::binary) << "two-different-bytes-here";
    }
    const auto second = Compiler::link_inputs_digest(command, { object });

    REQUIRE(first.has_value());
    REQUIRE(first == second);
}
