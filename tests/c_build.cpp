#include <catch2/catch_test_macros.hpp>

#include <Compiler/CBuild.h>
#include <Compiler/CompilerOptions.h>
#include <Compiler/HostTool.h>
#include <Compiler/TargetFacts.h>

#include <filesystem>
#include <vector>

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

// the key of the one object a build produced, read off the `--explain cache` line build_c_sources writes
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

std::vector<std::string> flag_values(const std::vector<std::string> &argv, const std::string &flag)
{
    std::vector<std::string> values;

    for (size_t i = 0; i + 1 < argv.size(); i++) {
        if (argv[i] == flag) {
            values.push_back(argv[i + 1]);
        }
    }

    return values;
}

bool argv_contains(const std::vector<std::string> &argv, const std::string &word)
{
    for (const std::string &arg : argv) {
        if (arg == word) {
            return true;
        }
    }

    return false;
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

TEST_CASE("coff_exports_from_nm keeps defined text and data", "[cbuild]")
{
    const std::string nm =
        "shim.o:\n"
        "00000000 T eco_shim_answer\n"
        "00000004 D eco_shim_flag\n"
        "00000008 B eco_shim_bss\n"
        "00000000 R eco_shim_ro\n"
        "         U printf\n"
        "00000010 T ?mangled@@YAXXZ\n"
        "00000020 T .text\n";

    const std::vector<std::string> names = Compiler::coff_exports_from_nm(nm);

    REQUIRE(names == std::vector<std::string>{
        "eco_shim_answer", "eco_shim_flag", "eco_shim_bss", "eco_shim_ro" });
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

TEST_CASE("a sysroot emmintrin.h does not shadow clang's", "[cbuild]")
{
    // the Windows bundle copies MSVC's include tree wholesale, so emmintrin.h
    // lands in sysroot/include/msvc. command-line `-isystem` of that tree used
    // to sit ahead of clang's resource directory and `_mm_*` became linker
    // symbols. this plants that colliding header; the resource include has to
    // stay first, and the sysroot has to stay `-isystem` rather than
    // `-isystem-after`, or a host VS beats the bundle
    ScopedProject project("sysroot_emmintrin");

    const fs::path sysroot = project.root() / "sysroot";
    const fs::path msvc = sysroot / "include" / "msvc";
    write_file(msvc / "emmintrin.h", "#error BUNDLED_EMMINTRIN\n");
    write_file(project.root() / "probe.c",
        "#include <emmintrin.h>\n"
        "int eco_sse_probe(void) { return 0; }\n");

    std::vector<std::string> argv = {
        "clang",
        "-c",
        "-o",
        (project.root() / "probe.o").string(),
        (project.root() / "probe.c").string(),
    };
    Compiler::append_windows_sysroot_cc_args(argv, sysroot);

    REQUIRE_FALSE(argv_contains(argv, "-isystem-after"));

    const std::vector<std::string> isystem = flag_values(argv, "-isystem");
    REQUIRE_FALSE(isystem.empty());
    REQUIRE(fs::is_regular_file(fs::path(isystem.front()) / "emmintrin.h"));
    REQUIRE(isystem.front() != msvc.string());

    bool saw_msvc = false;
    for (const std::string &dir : isystem) {
        if (dir == msvc.string()) {
            saw_msvc = true;
            break;
        }
    }
    REQUIRE(saw_msvc);

    const Compiler::CapturedProcess compiled = Compiler::run_captured(argv);
    INFO(compiled.output);
    REQUIRE(compiled.output.find("BUNDLED_EMMINTRIN") == std::string::npos);

    if (Compiler::TargetFacts::host().architecture == "x86_64") {
        REQUIRE(compiled.exit_code == 0);
    }
}

TEST_CASE("the Darwin SDK is asked for rather than assumed", "[cbuild][host]")
{
    // Homebrew clang cannot find libSystem without it; Apple clang often can.
    // both paths have to carry the SDK, because `echoc run` of a `#[cc:]`
    // module goes through clang -shared, not the ld fast path
    std::vector<std::string> argv = { "clang", "-shared", "-o", "libx.dylib" };
    Compiler::append_darwin_sdk_args(argv);

#if defined(__APPLE__)
    const fs::path sdk = Compiler::darwin_sdk_root();
    REQUIRE_FALSE(sdk.empty());
    REQUIRE(fs::is_directory(sdk));
    REQUIRE(flag_values(argv, "-isysroot") == std::vector<std::string>{ sdk.string() });
#else
    REQUIRE(Compiler::darwin_sdk_root().empty());
    REQUIRE(argv == std::vector<std::string>{ "clang", "-shared", "-o", "libx.dylib" });
#endif
}

TEST_CASE("a C shared library links against the host libc", "[cbuild]")
{
    // the loadable-library path `echoc run` uses. executable linking already
    // passed Darwin's SDK to `ld`; this is the clang -shared twin, and it
    // is what failed with `library 'System' not found` on Homebrew clang
    ScopedProject project("shared_libc");
    write_file(project.root() / "c" / "answer.c",
        "int eco_shim_answer(void) { return 42; }\n");

    Compiler::CBuildSpec spec;
    spec.module_name = "answer";
    spec.sources = { project.root() / "c" / "answer.c" };

    std::vector<std::string> explain;
    const Compiler::CBuildResult compiled = built(spec, project, explain);

    fs::path library;
    std::string error;
    REQUIRE(Compiler::build_c_shared_library(
        spec,
        compiled,
        {},
        project.build_dir(),
        project.root() / "scratch",
        library,
        error));
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(fs::is_regular_file(library));
}
