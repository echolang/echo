#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "subprocess.h"

// an unchanged `echoc build` builds nothing, and every input that moves rebuilds it.
// subprocess: the skip happens before a front end exists.

namespace
{

using EchoTests::ProcessResult;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("program_stamp", name)
    {};
};

// `--explain cache`: a pipe gets nothing from the progress row
bool skipped(const ProcessResult &built)
{
    return built.output.find("up to date") != std::string::npos;
}

// a C source behind a header: the stamp's second half
void write_stamped_project(const ScopedProject &project)
{
    write_file(project.root() / "module.eco",
        "#[module: \"stamped\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: include \"c\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n"
        "#[target: exe { name: \"other\", entry: \"src/other.eco\" }]\n");

    write_file(project.root() / "c/shim.h", "#define ANSWER 41\n");
    write_file(project.root() / "c/shim.c", "#include \"shim.h\"\nint shim_answer(void) { return ANSWER; }\n");

    write_file(project.root() / "src/api.eco", "extern { function shim_answer() : int32; }\n");
    write_file(project.root() / "src/main.eco", "echo shim_answer() + 1;\n");
    write_file(project.root() / "src/other.eco", "echo 'OTHER';\n");
}

std::string ran(const std::filesystem::path &binary)
{
    return EchoTests::run_binary(binary).output;
}

};

TEST_CASE("an unchanged program is not built again", "[program_stamp]")
{
    ScopedProject project("unchanged");
    write_stamped_project(project);

    const std::filesystem::path app = project.root() / "ecobuild/app";

    const ProcessResult first = project.echoc("build --explain cache --target app");
    INFO(first.output);
    REQUIRE(first.exit_code == 0);
    REQUIRE_FALSE(skipped(first));

    std::error_code ec;
    const auto written = std::filesystem::last_write_time(EchoTests::with_exe(app), ec);

    const ProcessResult second = project.echoc("build --explain cache --target app");
    INFO(second.output);
    REQUIRE(second.exit_code == 0);
    REQUIRE(skipped(second));

    // nothing touched it
    const bool untouched = std::filesystem::last_write_time(EchoTests::with_exe(app), ec) == written;
    REQUIRE(untouched);
    REQUIRE(ran(app).find("42") != std::string::npos);
}

TEST_CASE("every input of a program rebuilds it", "[program_stamp]")
{
    ScopedProject project("every_input");
    write_stamped_project(project);

    const std::filesystem::path app = project.root() / "ecobuild/app";

    REQUIRE(project.echoc("build --explain cache --target app").exit_code == 0);
    REQUIRE(skipped(project.echoc("build --explain cache --target app")));

    SECTION("an Echo source")
    {
        write_file(project.root() / "src/main.eco", "echo shim_answer() + 2;\n");

        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app")));
        REQUIRE(ran(app).find("43") != std::string::npos);
    }

    SECTION("a header only a C source includes, once and then not again")
    {
        // the C key is settled by the depfile: a stamp taken before the build would rebuild forever
        write_file(project.root() / "c/shim.h", "#define ANSWER 99\n");

        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app")));
        REQUIRE(ran(app).find("100") != std::string::npos);
        REQUIRE(skipped(project.echoc("build --explain cache --target app")));
    }

    SECTION("the build mode, the type tags, debug symbols and the allocation report")
    {
        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app --debug")));
        REQUIRE(skipped(project.echoc("build --explain cache --target app --debug")));
        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app --no-tbaa")));
        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app -g")));
        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app --explain memory")));
    }

    SECTION("the link line")
    {
        write_file(project.root() / "extra/empty.c", "int extra_symbol(void) { return 0; }\n");
        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app --link search:extra")));
    }

    SECTION("a deleted binary")
    {
        std::filesystem::remove(EchoTests::with_exe(app));

        REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app")));
        REQUIRE(EchoTests::file_exists(app));
    }
}

TEST_CASE("a dump is never skipped", "[program_stamp]")
{
    ScopedProject project("dump");
    write_stamped_project(project);

    REQUIRE(project.echoc("build --explain cache --target app").exit_code == 0);

    const ProcessResult dumped = project.echoc("build --explain cache --target app -p ir-units");
    REQUIRE(dumped.exit_code == 0);
    REQUIRE_FALSE(skipped(dumped));
    REQUIRE(dumped.output.find("define") != std::string::npos);
}

TEST_CASE("two programs of one module, and two outputs of one program, keep their own stamps", "[program_stamp]")
{
    ScopedProject project("separate_stamps");
    write_stamped_project(project);

    // both targets share every module key: nothing but the program's name tells them apart
    REQUIRE(project.echoc("build --explain cache --target app").exit_code == 0);

    const ProcessResult other = project.echoc("build --explain cache --target other");
    REQUIRE_FALSE(skipped(other));
    REQUIRE(ran(project.root() / "ecobuild/other").find("OTHER") != std::string::npos);

    const std::filesystem::path elsewhere = project.root() / "elsewhere";
    REQUIRE_FALSE(skipped(project.echoc("build --explain cache --target app -o " + elsewhere.string())));
    REQUIRE(skipped(project.echoc("build --explain cache --target app -o " + elsewhere.string())));
    REQUIRE(skipped(project.echoc("build --explain cache --target app")));
}

TEST_CASE("changing an embedded file of the program rebuilds it", "[program_stamp][embed]")
{
    ScopedProject project("embed_entry");

    write_file(project.root() / "module.eco",
        "#[module: \"embedcache\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"embedcache\", entry: \"src/main.eco\" }]\n");
    write_file(project.root() / "src/main.eco", "echo std::embed::text(\"page.txt\");\n");
    write_file(project.root() / "src/page.txt", "one");

    const std::filesystem::path app = project.root() / "ecobuild/embedcache";

    REQUIRE(project.echoc("build --explain cache --target embedcache").exit_code == 0);
    REQUIRE(ran(app).find("one") != std::string::npos);

    // first compile discovers the embed and writes the sidecar; the next key then
    // folds page.txt, so this rebuilds once
    REQUIRE(project.echoc("build --explain cache --target embedcache").exit_code == 0);
    REQUIRE(skipped(project.echoc("build --explain cache --target embedcache")));

    write_file(project.root() / "src/page.txt", "two");

    const ProcessResult after = project.echoc("build --explain cache --target embedcache");
    INFO(after.output);
    REQUIRE(after.exit_code == 0);
    REQUIRE_FALSE(skipped(after));
    REQUIRE(ran(app).find("two") != std::string::npos);
}

TEST_CASE("install after a skip still installs", "[program_stamp][install]")
{
    ScopedProject project("install_after_skip");
    write_stamped_project(project);

    const std::filesystem::path bin = project.root() / "bin";

    REQUIRE(project.echoc("build --explain cache --target app").exit_code == 0);

    const ProcessResult installed = project.echoc("install --target app --release --dir " + bin.string());
    INFO(installed.output);

    REQUIRE(installed.exit_code == 0);
    REQUIRE(EchoTests::file_exists(bin / "app"));
    REQUIRE(ran(bin / "app").find("42") != std::string::npos);
}
