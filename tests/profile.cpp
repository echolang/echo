#include <catch2/catch_test_macros.hpp>

#include <Compiler/CodegenTarget.h>
#include <Compiler/CommandLineOption.h>
#include <Compiler/ProfileDriver.h>
#include <Compiler/ProfileFile.h>
#include <Compiler/ProfileReport.h>
#include <Compiler/TargetFacts.h>

#include <eco_profile.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "subprocess.h"

// native `echoc run --profile` cases. sampled times are not stable, so this cannot
// be a corpus golden. file-format tests live in tests/profile_file.cpp

using EchoTests::ProcessResult;
using EchoTests::ScopedProject;

namespace
{
    std::filesystem::path find_ecoprof(const std::filesystem::path &root)
    {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
            if (entry.path().extension() == ".ecoprof") {
                return entry.path();
            }
        }
        return {};
    }

    bool strings_contain(const Compiler::ProfileFile &file, std::string_view needle)
    {
        return file.strings().find(needle) != std::string_view::npos;
    }

    uint64_t count_kind(const Compiler::ProfileFile &file, uint32_t kind)
    {
        uint64_t n = 0;
        for (const Compiler::ProfileEvent &event : file.events()) {
            if (file.event_kind(event.site_or_kind) == kind) {
                n += event.count;
            }
        }
        return n;
    }

    std::vector<std::string> call_site_names(const Compiler::ProfileFile &file)
    {
        std::vector<std::string> names;
        for (const Compiler::ProfileSite &site : file.sites()) {
            if (site.kind == ECO_PROF_KIND_CALL) {
                names.emplace_back(file.string_at(site.type_name));
            }
        }
        return names;
    }

    bool names_contain(const std::vector<std::string> &names, std::string_view needle)
    {
        for (const std::string &name : names) {
            if (name.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
}

#if !defined(_WIN32)
TEST_CASE("echoc run --profile writes a file", "[profile]")
{
    ScopedProject project("profile", "writes_a_file");
    EchoTests::write_file(project.root() / "main.eco", "echo 1;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.output.find("[profile]") != std::string::npos);

    const std::filesystem::path profile = project.root() / "main.ecoprof";
    REQUIRE(EchoTests::file_exists(profile));
    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(profile, file, error));
    REQUIRE(file.header().status == 0);
    REQUIRE(file.symbolized());
}

TEST_CASE("an array push is attributed to the allocating path", "[profile]")
{
    ScopedProject project("profile", "array_push");
    EchoTests::write_file(
        project.root() / "main.eco",
        "function spawn() : void {\n"
        "    array<int32> $a;\n"
        "    $a[] = 1;\n"
        "}\n"
        "spawn();\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    const std::filesystem::path profile = find_ecoprof(project.root());
    REQUIRE_FALSE(profile.empty());

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(profile, file, error));
    REQUIRE(file.symbolized());

    uint64_t allocs = 0;
    for (const auto &event : file.events()) {
        const uint32_t kind = file.event_kind(event.site_or_kind);
        if (kind == ECO_PROF_KIND_ALLOC || kind == ECO_PROF_KIND_REALLOC) {
            allocs += event.count;
        }
    }
    REQUIRE(allocs > 0);
    REQUIRE(strings_contain(file, "spawn"));

    std::ostringstream folded;
    REQUIRE(Compiler::ProfileReport::write_folded(file, Compiler::ProfileFoldKind::t_allocs, folded, error));
    REQUIRE(folded.str().find("spawn") != std::string::npos);
    REQUIRE(folded.str().find("__eco_") == std::string::npos);
}

TEST_CASE("env::exit and die write status 1 and 2", "[profile]")
{
    ScopedProject project("profile", "status");
    EchoTests::write_file(project.root() / "exit.eco", "std::env::exit(7);\n");
    EchoTests::write_file(project.root() / "die.eco", "die('gone');\n");

    const ProcessResult exited = project.echoc("run --profile --release exit.eco");
    REQUIRE(exited.exit_code != 0);
    const std::filesystem::path exit_profile = project.root() / "exit.ecoprof";
    REQUIRE(EchoTests::file_exists(exit_profile));
    Compiler::ProfileFile exit_file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(exit_profile, exit_file, error));
    REQUIRE(exit_file.header().status == ECO_PROF_STATUS_EXIT);

    const ProcessResult died = project.echoc("run --profile --release die.eco");
    REQUIRE(died.exit_code != 0);
    const std::filesystem::path die_profile = project.root() / "die.ecoprof";
    REQUIRE(EchoTests::file_exists(die_profile));
    Compiler::ProfileFile die_file;
    REQUIRE(Compiler::ProfileFile::open(die_profile, die_file, error));
    REQUIRE(die_file.header().status == ECO_PROF_STATUS_ABORT);
}

TEST_CASE("a sampler records samples in a busy loop", "[profile]")
{
    ScopedProject project("profile", "sampler");
    EchoTests::write_file(
        project.root() / "main.eco",
        "extern {\n"
        "    function rand() : int32;\n"
        "}\n"
        "int32 $s = 0;\n"
        "for (int32 $i = 0; $i < 3000000; $i = $i + 1) {\n"
        "    $s = $s + rand();\n"
        "}\n"
        "echo $s;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    const std::filesystem::path profile = find_ecoprof(project.root());
    REQUIRE_FALSE(profile.empty());

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(profile, file, error));
    uint64_t samples = 0;
    for (const auto &sample : file.samples()) {
        samples += sample.samples;
    }
    REQUIRE(samples > 0);
}

TEST_CASE("a reassignment after init is a bytewise copy", "[profile]")
{
    ScopedProject project("profile", "vec3_copy");
    EchoTests::write_file(
        project.root() / "main.eco",
        "struct Vec3 {\n"
        "    float32 $x;\n"
        "    float32 $y;\n"
        "    float32 $z;\n"
        "}\n"
        "Vec3 $a = Vec3(1.0, 2.0, 3.0);\n"
        "Vec3 $b = Vec3(0.0, 0.0, 0.0);\n"
        "$b = $a;\n"
        "echo $b->x;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    const std::filesystem::path profile = find_ecoprof(project.root());
    REQUIRE_FALSE(profile.empty());

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(profile, file, error));

    uint64_t copies = 0;
    uint64_t copy_bytes = 0;
    for (const auto &event : file.events()) {
        const uint32_t kind = file.event_kind(event.site_or_kind);
        if (kind == ECO_PROF_KIND_COPY_BYTES) {
            copies += event.count;
            copy_bytes += event.bytes;
        }
    }
    REQUIRE(copies > 0);
    REQUIRE(copy_bytes > 0);
}

TEST_CASE("initializing a struct from a local is a bytewise copy", "[profile]")
{
    ScopedProject project("profile", "vec3_init_copy");
    EchoTests::write_file(
        project.root() / "main.eco",
        "struct Vec3 {\n"
        "    float32 $x;\n"
        "    float32 $y;\n"
        "    float32 $z;\n"
        "}\n"
        "Vec3 $a = Vec3(1.0, 2.0, 3.0);\n"
        "Vec3 $b = $a;\n"
        "echo $a->x;\n"
        "echo $b->x;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", file, error));
    REQUIRE(count_kind(file, ECO_PROF_KIND_COPY_BYTES) > 0);
}

TEST_CASE("profile is refused for a lib, wasi, and ios", "[profile]")
{
    ScopedProject lib_project("profile", "lib_refused");
    EchoTests::write_file(
        lib_project.root() / "module.eco",
        "#[module: \"onlylib\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: lib { name: \"greeter\" }]\n");
    EchoTests::write_file(
        lib_project.root() / "src/lib.eco",
        "#[export]\n"
        "public function add(int32 $a, int32 $b) : int32 { return $a + $b; }\n");

    const ProcessResult lib = lib_project.echoc("build --profile --no-stdlib");
    INFO(lib.output);
    REQUIRE(lib.exit_code != 0);
    REQUIRE(lib.output.find("lib has none") != std::string::npos);

    ScopedProject cross("profile", "cross_refused");
    EchoTests::write_file(cross.root() / "main.eco", "echo 1;\n");

    const ProcessResult wasi = cross.echoc("build --profile --target-os wasi --no-stdlib -o out main.eco");
    INFO(wasi.output);
    REQUIRE(wasi.exit_code != 0);
    REQUIRE(wasi.output.find("native host program") != std::string::npos);

    const ProcessResult ios = cross.echoc("build --profile --target-os ios --no-stdlib -o out main.eco");
    INFO(ios.output);
    REQUIRE(ios.exit_code != 0);
    REQUIRE(ios.output.find("native host program") != std::string::npos);
}

TEST_CASE("profile is refused for ios facts on a host codegen row", "[profile]")
{
    // `--target-os ios` off Darwin keeps the host codegen row; facts is the ask
    Compiler::TargetFacts ios;
    std::string error;
    REQUIRE(Compiler::TargetFacts::resolve("ios", "", {}, ios, error));

    const auto issue = Compiler::profile_refuse_target(Compiler::CodegenTarget(), ios);
    REQUIRE(issue.has_value());
    REQUIRE(std::string(issue->message).find("native host program") != std::string::npos);
}

TEST_CASE("a user copy constructor is a COPY_CTOR event", "[profile]")
{
    ScopedProject project("profile", "copy_ctor");
    EchoTests::write_file(
        project.root() / "main.eco",
        "struct Counted {\n"
        "    int32 $v;\n"
        "    constructor(int32 $v) { $this->v = $v; }\n"
        "    constructor(const Counted& $other) { $this->v = $other->v + 1; }\n"
        "}\n"
        "Counted $a = Counted(1);\n"
        "Counted $b = Counted(0);\n"
        "$b = $a;\n"
        "echo $a->v;\n"
        "echo $b->v;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", file, error));
    REQUIRE(count_kind(file, ECO_PROF_KIND_COPY_CTOR) > 0);
}

TEST_CASE("an inline-array copy is a COPY_CTOR event", "[profile]")
{
    ScopedProject project("profile", "inline_array_copy");
    EchoTests::write_file(
        project.root() / "main.eco",
        "struct Item {\n"
        "    string $n;\n"
        "}\n"
        "Item[1] $a = [Item(\"a\")];\n"
        "Item[1] $b = [Item(\"b\")];\n"
        "$b = $a;\n"
        "echo $b[0]->n;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", file, error));
    REQUIRE(count_kind(file, ECO_PROF_KIND_COPY_CTOR) > 0);
}

TEST_CASE("a leaked alloc is still live at exit", "[profile]")
{
    ScopedProject project("profile", "live_at_exit");
    EchoTests::write_file(
        project.root() / "main.eco",
        "ptr<int32> $p = mem::alloc<int32>(4);\n"
        "echo 1;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", file, error));
    uint64_t live_bytes = 0;
    for (const auto &event : file.live()) {
        live_bytes += event.bytes;
    }
    REQUIRE(live_bytes > 0);
}

TEST_CASE("profile-calls records exact call sites, and stdlib is opt-in", "[profile]")
{
    ScopedProject project("profile", "calls");
    EchoTests::write_file(
        project.root() / "main.eco",
        "function spawn() : void {\n"
        "    echo std::env::pid();\n"
        "}\n"
        "spawn();\n");

    const ProcessResult user = project.echoc("run --profile-calls --release main.eco");
    INFO(user.output);
    REQUIRE(user.exit_code == 0);
    REQUIRE(user.output.find("Calls") != std::string::npos);
    REQUIRE(user.output.find("spawn") != std::string::npos);

    Compiler::ProfileFile user_file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", user_file, error));
    REQUIRE((user_file.header().flags & ECO_PROF_FLAG_CALLS) != 0);
    REQUIRE_FALSE(user_file.cct().empty());
    const auto user_names = call_site_names(user_file);
    REQUIRE(names_contain(user_names, "spawn"));
    REQUIRE_FALSE(names_contain(user_names, "pid"));

    const ProcessResult with_stdlib = project.echoc(
        "run --profile-calls --profile-stdlib --release main.eco");
    INFO(with_stdlib.output);
    REQUIRE(with_stdlib.exit_code == 0);

    Compiler::ProfileFile stdlib_file;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", stdlib_file, error));
    const auto stdlib_names = call_site_names(stdlib_file);
    REQUIRE(names_contain(stdlib_names, "spawn"));
    REQUIRE(names_contain(stdlib_names, "pid"));

    const ProcessResult folded = project.echoc("profile --folded calls main.ecoprof");
    REQUIRE(folded.exit_code == 0);
    REQUIRE(folded.output.find("spawn") != std::string::npos);
    REQUIRE(folded.output.find("\n? ") == std::string::npos);
    REQUIRE_FALSE(folded.output.starts_with("? "));
}

TEST_CASE("ECHO_PROFILE_OUT and a project layout choose the file", "[profile]")
{
    ScopedProject project("profile", "out_path");
    EchoTests::write_file(project.root() / "main.eco", "echo 1;\n");

    const std::filesystem::path named = project.root() / "custom.ecoprof";
    const ProcessResult named_run = project.echoc(
        "run --profile --release main.eco",
        project.root(),
        { { "ECHO_PROFILE_OUT", named.string() } });
    INFO(named_run.output);
    REQUIRE(named_run.exit_code == 0);
    REQUIRE(EchoTests::file_exists(named));
    REQUIRE(named_run.output.find("custom.ecoprof") != std::string::npos);

    ScopedProject app("profile", "build_dir_profile");
    EchoTests::write_file(
        app.root() / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"src/*.eco\"]\n"
        "#[target: exe { name: \"app\", entry: \"src/main.eco\" }]\n");
    EchoTests::write_file(app.root() / "src/main.eco", "echo 1;\n");
    const ProcessResult laid_out = app.echoc("run --profile --release");
    INFO(laid_out.output);
    REQUIRE(laid_out.exit_code == 0);
    REQUIRE(EchoTests::file_exists(app.root() / "ecobuild" / "profile" / "app.ecoprof"));
}

TEST_CASE("profile refuses a JIT run", "[profile]")
{
    ScopedProject project("profile", "jit_refused");
    EchoTests::write_file(project.root() / "main.eco", "echo 1;\n");

    const ProcessResult result = project.echoc("run --profile --explain prune main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code != 0);
    REQUIRE(result.output.find("native run") != std::string::npos);
}

TEST_CASE("returning a last-read struct is not a bytewise copy", "[profile]")
{
    ScopedProject project("profile", "return_handover");
    EchoTests::write_file(
        project.root() / "main.eco",
        "struct Vec3 {\n"
        "    float32 $x;\n"
        "    float32 $y;\n"
        "    float32 $z;\n"
        "}\n"
        "function take() : Vec3 {\n"
        "    Vec3 $a = Vec3(1.0, 2.0, 3.0);\n"
        "    return $a;\n"
        "}\n"
        "Vec3 $b = take();\n"
        "echo $b->x;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);
    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", file, error));
    REQUIRE(count_kind(file, ECO_PROF_KIND_COPY_BYTES) == 0);
}

TEST_CASE("a match arm that copies a struct is a bytewise copy", "[profile]")
{
    ScopedProject project("profile", "match_arm_copy");
    EchoTests::write_file(
        project.root() / "main.eco",
        "struct Vec3 {\n"
        "    float32 $x;\n"
        "    float32 $y;\n"
        "    float32 $z;\n"
        "}\n"
        "enum Side {\n"
        "    case left;\n"
        "    case right;\n"
        "}\n"
        "Vec3 $x = Vec3(1.0, 2.0, 3.0);\n"
        "Side $s = Side::left;\n"
        "Vec3 $b = match ($s) {\n"
        "    .left => $x,\n"
        "    .right => Vec3(0.0, 0.0, 0.0),\n"
        "};\n"
        "echo $x->x;\n"
        "echo $b->x;\n");

    const ProcessResult result = project.echoc("run --profile --release main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code == 0);
    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(project.root() / "main.ecoprof", file, error));
    REQUIRE(count_kind(file, ECO_PROF_KIND_COPY_BYTES) > 0);
}
#endif

#if defined(_WIN32)
TEST_CASE("profile is refused on Windows", "[profile]")
{
    ScopedProject project("profile", "windows_refused");
    EchoTests::write_file(project.root() / "main.eco", "echo 1;\n");

    const ProcessResult result = project.echoc("run --profile main.eco");
    INFO(result.output);
    REQUIRE(result.exit_code != 0);
    REQUIRE(result.output.find("not available on Windows") != std::string::npos);
}
#endif
