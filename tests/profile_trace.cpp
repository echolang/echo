#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "subprocess.h"

// Chrome Trace Event spelling cannot be a corpus golden: write_trace needs a path, and a
// default filename would drop a file into the tests binary's working directory for every
// case. This suite gives the program a scratch path and reads the file back.
//
// the contract is complete events (`ph:"X"`), not instant events (`ph:"i"` / `ph:"I"`).
// those latter are the allocation trace, a different pairing story.

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("profile_trace", name)
    {};
};

std::string read_file(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

};

TEST_CASE("write_trace emits complete events, not instant ones", "[profile]")
{
    ScopedProject project("complete_events");

    write_file(project.root() / "app.eco", R"(
use std::profile;

profile::start();
{
    profile::zone $z = profile::zone('simulate');
}
profile::write_trace('frames.json');
echo 1;
)");

    const ProcessResult result = project.echoc("run app.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("1") != std::string::npos);

    const std::string json = read_file(project.root() / "frames.json");
    REQUIRE(json.find("\"ph\":\"X\"") != std::string::npos);
    REQUIRE(json.find("\"name\":\"simulate\"") != std::string::npos);
    REQUIRE(json.find("\"ph\":\"i\"") == std::string::npos);
    REQUIRE(json.find("\"ph\":\"I\"") == std::string::npos);
}
