#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "subprocess.h"

// `public: true` on a dependency's exe: a consumer may ask for it by name. ordinary builds ignore it.
// subprocess: a corpus case always appends its own `.eco`.

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("public_tools", name)
    {};
};

// one public tool, one private exe, one test: the three names a consumer could trip over
void write_offering_library(const std::filesystem::path &dir, const std::string &module)
{
    write_file(dir / "module.eco",
        "#[module: \"" + module + "\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"gen\", entry: \"cli/main.eco\", public: true }] {\n"
        "    #[sources: \"cli/*.eco\"]\n"
        "}\n"
        "#[target: exe { name: \"internal\", entry: \"src/internal_main.eco\" }]\n"
        "#[target: test]\n");

    write_file(dir / "src/api.eco",
        "namespace " + module + ";\n"
        "public function answer() : int32 { return 42; }\n");

    write_file(dir / "src/internal_main.eco", "echo 'INTERNAL';\n");
    write_file(dir / "cli/main.eco", "echo '" + module + " GEN RAN';\n");
}

void write_consumer(const std::filesystem::path &dir, const std::string &depends)
{
    write_file(dir / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n" + depends);

    write_file(dir / "src/main.eco", "echo 'APP RAN';\n");
}

std::string settled_spelling(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::filesystem::path settled = std::filesystem::weakly_canonical(path, ec);
    return (ec ? path : settled).string();
}

};

TEST_CASE("-p tools names what a direct dependency offers, and only that", "[public_tools]")
{
    ScopedProject project("names_the_offer");

    write_offering_library(project.root() / "restgen", "restgen");
    write_consumer(project.root() / "app", "#[depends: \"../restgen\"]\n");

    const ProcessResult listed = project.echoc("build -p tools", project.root() / "app");
    INFO(listed.output);

    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.output.find("\"public_tools\"") != std::string::npos);

    // the bare name selects it, because nothing else on offer shares it, and the qualified one always does
    REQUIRE(listed.output.find("\"refs\": [\"gen\", \"restgen:gen\"]") != std::string::npos);

    // the exe it does not offer and its test target are nowhere in the answer
    REQUIRE(listed.output.find("internal") == std::string::npos);
    REQUIRE(listed.output.find("tests") == std::string::npos);
}

TEST_CASE("a bare name two dependencies offer is withheld from both", "[public_tools]")
{
    ScopedProject project("shared_bare_name");

    write_offering_library(project.root() / "one", "one");
    write_offering_library(project.root() / "two", "two");
    write_consumer(project.root() / "app", "#[depends: \"../one\"]\n#[depends: \"../two\"]\n");

    const ProcessResult listed = project.echoc("build -p tools", project.root() / "app");
    INFO(listed.output);

    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.output.find("\"refs\": [\"one:gen\"]") != std::string::npos);
    REQUIRE(listed.output.find("\"refs\": [\"two:gen\"]") != std::string::npos);
}

TEST_CASE("the binary -p tools reports is the one a build of the tool writes", "[public_tools]")
{
    ScopedProject project("binary_path_agrees");

    write_offering_library(project.root() / "restgen", "restgen");
    write_consumer(project.root() / "app", "#[depends: \"../restgen\"]\n");

    const ProcessResult listed = project.echoc("build -p tools", project.root() / "app");
    REQUIRE(listed.exit_code == 0);

    const std::string marker = "\"binary\": \"";
    const size_t at = listed.output.find(marker);
    REQUIRE(at != std::string::npos);

    const size_t start = at + marker.size();
    const std::string reported = listed.output.substr(start, listed.output.find('"', start) - start);

    // what `epm run` does: a build rooted at the dependency
    const ProcessResult built = project.echoc("build -m ../restgen --target gen", project.root() / "app");
    INFO(built.output);
    REQUIRE(built.exit_code == 0);

    REQUIRE(EchoTests::file_exists(reported));

    const ProcessResult ran = EchoTests::run_binary(reported);
    REQUIRE(ran.output.find("restgen GEN RAN") != std::string::npos);
}

TEST_CASE("offering a tool changes nothing about a consumer's build", "[public_tools]")
{
    ScopedProject project("offering_is_not_building");

    write_offering_library(project.root() / "restgen", "restgen");
    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[depends: \"../restgen\"]\n");
    write_file(project.root() / "app/src/main.eco", "echo restgen::answer();\n");

    // the tool's CLI is broken Echo: reading its scope would refuse the consumer
    write_file(project.root() / "restgen/cli/broken.eco", "this is not echo ][ {{{\n");

    const ProcessResult ran = project.echoc("run", project.root() / "app");
    INFO(ran.output);

    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("42") != std::string::npos);
    REQUIRE_FALSE(EchoTests::file_exists(project.root() / "restgen/ecobuild/gen"));
}

TEST_CASE("-p tools reads edges, not sources", "[public_tools]")
{
    // a hook may write the files a pattern needs, so listing tools must not need them
    ScopedProject project("edges_not_sources");

    write_offering_library(project.root() / "restgen", "restgen");
    write_file(project.root() / "app/module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/generated/*.eco\"]\n"
        "#[depends: \"../restgen\"]\n");

    const ProcessResult listed = project.echoc("build -p tools", project.root() / "app");
    INFO(listed.output);

    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.output.find("restgen:gen") != std::string::npos);
}

TEST_CASE("a vendored package's tool is found through the package directory", "[public_tools][packages]")
{
    ScopedProject project("vendored_tool");

    write_offering_library(project.root() / "app/vendor/restgen", "restgen");
    const std::string requires_restgen =
        "#[requires: \"restgen\" { version: \"^1\", source: git \"https://example.com/restgen\" }]\n";
    write_consumer(project.root() / "app", requires_restgen);

    const ProcessResult listed = project.echoc("build -p tools", project.root() / "app");
    INFO(listed.output);

    REQUIRE(listed.exit_code == 0);
    REQUIRE(listed.output.find(
        "\"package_dir\": \"" + settled_spelling(project.root() / "app/vendor") + "\"") != std::string::npos);
    REQUIRE(listed.output.find("\"refs\": [\"gen\", \"restgen:gen\"]") != std::string::npos);
}

TEST_CASE("-p tools answers for one module", "[public_tools]")
{
    ScopedProject project("one_module");

    const ProcessResult nothing = project.echoc("build -p tools");

    REQUIRE(nothing.exit_code == 1);
    REQUIRE(nothing.output.find("needs a '-m' or a module.eco") != std::string::npos);
}
