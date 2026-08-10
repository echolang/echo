#include <catch2/catch_test_macros.hpp>

#include <Compiler/CBuild.h>
#include <Compiler/CompilerOptions.h>

#include <filesystem>

#include "subprocess.h"

// what a `#[cc: ...]` value means, and what the C object cache is a function of.
//
// the interesting half is the last one. A C translation unit's inputs are not its source file: they are its
// source file and every header it reached, and nothing in a manifest names those. clang's own depfile is
// what closes that, and the property this suite exists to hold is that editing *only* a header moves the
// key - the module cache's one silent failure mode, reproduced in a second store if this is not checked.

namespace fs = std::filesystem;

namespace
{

using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("c_build", name)
    {};
};

std::string parsed_value(
    const std::string &spelled, const fs::path &base, Compiler::CcScheme &out_scheme)
{
    std::string value;
    std::string error;

    REQUIRE(Compiler::parse_cc_requirement(spelled, base, out_scheme, value, error));
    REQUIRE(error.empty());

    return value;
}

std::string refusal_of(const std::string &spelled, const fs::path &base)
{
    Compiler::CcScheme scheme = Compiler::CcScheme::t_sources;
    std::string value;
    std::string error;

    REQUIRE_FALSE(Compiler::parse_cc_requirement(spelled, base, scheme, value, error));

    return error;
}

// one module's C build, over a shim this suite writes
Compiler::CBuildSpec spec_for(const ScopedProject &project, const std::string &module_name)
{
    Compiler::CBuildSpec spec;
    spec.module_name = module_name;
    spec.sources = { project.root() / "c" / "shim.c" };
    spec.includes = { project.root() / "c" };

    return spec;
}

// the key of the one object a build produced, read off the `--explain-cache` line build_c_sources writes
std::string key_of(const std::vector<std::string> &explain)
{
    REQUIRE(explain.size() == 1);

    std::istringstream fields(explain.front());
    std::string name;
    std::string key;
    fields >> name >> key;

    return key;
}

Compiler::CBuildResult built(
    const Compiler::CBuildSpec &spec, const ScopedProject &project, std::vector<std::string> &explain)
{
    Compiler::CompilerOptions options;
    Compiler::CBuildResult result;
    std::string error;

    REQUIRE(Compiler::build_c_sources(
        spec, options, project.build_dir(), project.root() / "scratch", explain, result, error));
    REQUIRE(error.empty());

    return result;
}

void write_shim(const ScopedProject &project, const std::string &header_body)
{
    write_file(project.root() / "c" / "shim.h", header_body);
    write_file(project.root() / "c" / "shim.c",
        "#include \"shim.h\"\n"
        "int eco_shim_answer(void) { return ANSWER; }\n");
}

};

TEST_CASE("a sources pattern is kept as written", "[cbuild]")
{
    Compiler::CcScheme scheme = Compiler::CcScheme::t_include;

    // expanding it has one owner and it is Parser::expand_source_pattern - a second expander here is how
    // `*` would come to mean one thing in `#[sources:]` and another in `#[cc:]`
    REQUIRE(parsed_value("sources:c/*.c", fs::current_path(), scheme) == "c/*.c");
    REQUIRE(scheme == Compiler::CcScheme::t_sources);
}

TEST_CASE("an include path resolves against the manifest and must exist", "[cbuild]")
{
    ScopedProject project("include_path");

    fs::create_directories(project.root() / "c" / "include");

    Compiler::CcScheme scheme = Compiler::CcScheme::t_sources;
    const std::string resolved = parsed_value("include:c/include", project.root(), scheme);

    REQUIRE(scheme == Compiler::CcScheme::t_include);
    REQUIRE(fs::path(resolved).is_absolute());
    REQUIRE(fs::equivalent(resolved, project.root() / "c" / "include"));

    REQUIRE(refusal_of("include:nowhere", project.root()).find("is not a directory") != std::string::npos);
}

TEST_CASE("an unknown C build scheme is refused", "[cbuild]")
{
    const std::string refusal = refusal_of("headers:c/include", fs::current_path());

    REQUIRE(refusal.find("'headers' is not a C build scheme") != std::string::npos);
    REQUIRE(refusal.find("sources, include, define, flag") != std::string::npos);
}

TEST_CASE("a C object is reused when nothing changed", "[cbuild][cache]")
{
    ScopedProject project("reuse");

    write_shim(project, "#define ANSWER 42\n");

    const Compiler::CBuildSpec spec = spec_for(project, "shimtest");

    std::vector<std::string> first;
    const Compiler::CBuildResult one = built(spec, project, first);

    REQUIRE(one.objects.size() == 1);
    REQUIRE(first.front().find("miss") != std::string::npos);

    std::vector<std::string> second;
    const Compiler::CBuildResult two = built(spec, project, second);

    REQUIRE(second.front().find("hit") != std::string::npos);
    REQUIRE(two.objects.front() == one.objects.front());
}

TEST_CASE("editing the source moves the key", "[cbuild][cache]")
{
    ScopedProject project("changed_source");

    write_shim(project, "#define ANSWER 42\n");

    const Compiler::CBuildSpec spec = spec_for(project, "shimtest");

    std::vector<std::string> first;
    built(spec, project, first);

    write_file(project.root() / "c" / "shim.c",
        "#include \"shim.h\"\n"
        "int eco_shim_answer(void) { return ANSWER + 1; }\n");

    std::vector<std::string> second;
    built(spec, project, second);

    REQUIRE(key_of(first) != key_of(second));
    REQUIRE(second.front().find("miss") != std::string::npos);
}

TEST_CASE("editing only a header moves the key", "[cbuild][cache]")
{
    ScopedProject project("changed_header");

    write_shim(project, "#define ANSWER 42\n");

    const Compiler::CBuildSpec spec = spec_for(project, "shimtest");

    // the first build has no depfile and always runs, which is what makes the second one able to see the
    // header at all
    std::vector<std::string> first;
    built(spec, project, first);

    std::vector<std::string> unchanged;
    built(spec, project, unchanged);
    REQUIRE(unchanged.front().find("hit") != std::string::npos);

    // **the source is untouched.** without the depfile this is a stale object and nothing anywhere says so
    write_file(project.root() / "c" / "shim.h", "#define ANSWER 43\n");

    std::vector<std::string> after;
    built(spec, project, after);

    REQUIRE(key_of(unchanged) != key_of(after));
    REQUIRE(after.front().find("miss") != std::string::npos);
}

TEST_CASE("a define changes the key without touching a file", "[cbuild][cache]")
{
    ScopedProject project("changed_define");

    write_shim(project, "#define ANSWER 42\n");

    Compiler::CBuildSpec spec = spec_for(project, "shimtest");

    std::vector<std::string> first;
    built(spec, project, first);

    spec.defines.push_back("EXTRA=1");

    std::vector<std::string> second;
    built(spec, project, second);

    REQUIRE(key_of(first) != key_of(second));
}

TEST_CASE("an unwritable store compiles to scratch rather than failing", "[cbuild][cache]")
{
    ScopedProject project("no_store");

    write_shim(project, "#define ANSWER 42\n");

    const Compiler::CBuildSpec spec = spec_for(project, "shimtest");

    Compiler::CompilerOptions options;
    Compiler::CBuildResult result;
    std::vector<std::string> explain;
    std::string error;

    // no cache directory at all, which is what an unwritable one amounts to. A cache is an optimization,
    // so the only correct answer is to compile and keep nothing
    REQUIRE(Compiler::build_c_sources(
        spec, options, fs::path(), project.root() / "scratch", explain, result, error));

    REQUIRE(error.empty());
    REQUIRE(result.objects.size() == 1);
    REQUIRE(fs::is_regular_file(result.objects.front()));
}

TEST_CASE("a module with no C sources builds nothing", "[cbuild]")
{
    ScopedProject project("empty");

    Compiler::CBuildSpec spec;
    spec.module_name = "plain";

    Compiler::CompilerOptions options;
    Compiler::CBuildResult result;
    std::vector<std::string> explain;
    std::string error;

    REQUIRE(spec.empty());
    REQUIRE(Compiler::build_c_sources(
        spec, options, project.build_dir(), project.root() / "scratch", explain, result, error));

    REQUIRE(result.objects.empty());
    REQUIRE(explain.empty());
}
