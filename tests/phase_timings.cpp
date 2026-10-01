#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <string>

// `--explain time` is a tree of names, not a golden of milliseconds. a subprocess is what pins that
// the nested rows actually appear on an invocation, which is the thing a unit test of PhaseTimings
// itself cannot see - enable() is a process-wide flag the driver sets

TEST_CASE("explain time nests semantic passes and emit objects", "[cli]")
{
    EchoTests::ScopedProject project("phase_timings", "nested");
    EchoTests::write_file(project.root() / "main.eco", "echo 1;\n");

    const EchoTests::ProcessResult result = project.echoc(
        "build --no-stdlib --explain time --build-dir build -o app main.eco");

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("[timings]") != std::string::npos);
    REQUIRE(result.output.find("semantic passes") != std::string::npos);
    REQUIRE(result.output.find("constants") != std::string::npos);
    REQUIRE(result.output.find("instances") != std::string::npos);
    REQUIRE(result.output.find("pointers") != std::string::npos);
    REQUIRE(result.output.find("access") != std::string::npos);
    REQUIRE(result.output.find("types") != std::string::npos);
    REQUIRE(result.output.find("emit objects") != std::string::npos);
    REQUIRE(result.output.find("optimize") != std::string::npos);
    REQUIRE(result.output.find("machine code") != std::string::npos);
}
