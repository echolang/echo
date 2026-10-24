#include <catch2/catch_test_macros.hpp>

#include <Compiler/DependencyTrust.h>
#include <Compiler/DriverOptions.h>
#include <Compiler/LinkRequirement.h>
#include <Compiler/Lsp/LspSession.h>
#include <Parser/ManifestParser.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "subprocess.h"

// what a dependency may do to the machine that builds it, and what a shipped build directory may make
// the compiler do. one owner: `Compiler::module_is_trusted` and the refusals beside it, and
// `Compiler::ArtifactSeal` for the cache

namespace fs = std::filesystem;

namespace
{

using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::write_file;

class ScopedProject : public EchoTests::ScopedProject
{
public:
    explicit ScopedProject(const std::string &name) :
        EchoTests::ScopedProject("dependency_trust", name)
    {};
};

// a consumer `app` depending on a library `dep`, whose manifest carries `extra` and whose C, when it has
// any, is one trivial translation unit
void write_project(const fs::path &root, const std::string &extra)
{
    write_file(root / "dep" / "module.eco",
        "#[module: \"dep\"]\n"
        "#[sources: \"*.eco\"]\n"
        + extra);

    write_file(root / "dep" / "lib.eco",
        "public function twice(int32 $x) : int32\n"
        "{\n"
        "    return $x * 2;\n"
        "}\n");

    write_file(root / "dep" / "c" / "h.c", "int dep_helper(void) { return 1; }\n");

    write_file(root / "app" / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"*.eco\"]\n"
        "#[depends: \"../dep\"]\n");

    write_file(root / "app" / "main.eco", "echo twice(21);\n");
}

std::string read_text(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// the `--explain cache` line for one module, or empty
std::string cache_line_for(const std::string &output, const std::string &module)
{
    const std::string needle = "  " + module + "  ";
    const size_t at = output.find(needle);

    if (at == std::string::npos) {
        return {};
    }

    return output.substr(at, output.find('\n', at) - at);
}

fs::path only_object_in(const fs::path &directory)
{
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(directory, ec)) {
        if (entry.path().extension() == ".o") {
            return entry.path();
        }
    }

    return {};
}

};

TEST_CASE("a dependency's C flags are held to the deny-list", "[trust]")
{
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-Wall").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-Wno-unused-function").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-Wframe-larger-than=4096").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-O3").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-g").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-std=c11").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-mavx2").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-march=armv8-a").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-fno-strict-aliasing").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-fvisibility=hidden").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-DFOO=1").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-fobjc-arc").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-fobjc-arc-exceptions").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-flto").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-ObjC").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_flag_refusal("-fopenmp").has_value());

    // plugins, response files, path-valued flags, outputs. `-fobjc-arc` is not `-F`
    REQUIRE(Compiler::dependency_cc_flag_refusal("-fplugin=/tmp/evil.so").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-fpass-plugin=/tmp/evil.so").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-Xclang").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-mllvm").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-mllvm=hidden-option").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("@/tmp/args").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("--config=/tmp/x.cfg").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-B/tmp").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-o").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-include/etc/passwd").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-I/etc").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-Wp,-MD,/tmp/x").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-Wl,-rpath,/tmp").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-fprofile-generate=/tmp").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-ftime-trace=/tmp/x").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-fsanitize-ignorelist=/tmp/x").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-load").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-working-directory=/tmp").has_value());
    REQUIRE(Compiler::dependency_cc_flag_refusal("-gen-reproducer").has_value());

    REQUIRE_FALSE(Compiler::dependency_cc_define_refusal("FOO").has_value());
    REQUIRE_FALSE(Compiler::dependency_cc_define_refusal("FOO=-fplugin=x").has_value());
    REQUIRE(Compiler::dependency_cc_define_refusal("-fplugin").has_value());
}

TEST_CASE("a dependency's lib name cannot be a path", "[trust]")
{
    Parser::ModuleManifest manifest;
    manifest.name = "dep";
    manifest.directory = fs::current_path();

    Parser::ModuleContribution contribution;
    Compiler::LinkRequirement lib;
    lib.scheme = Compiler::LinkScheme::t_library;
    lib.value = "/tmp/libevil.so";
    contribution.link.push_back(lib);

    REQUIRE(Compiler::dependency_contribution_refusal(manifest, contribution).has_value());

    contribution.link[0].value = ":libevil.so";
    REQUIRE(Compiler::dependency_contribution_refusal(manifest, contribution).has_value());

    contribution.link[0].value = "ssl";
    REQUIRE_FALSE(Compiler::dependency_contribution_refusal(manifest, contribution).has_value());

    contribution.link[0].file = "/tmp/libssl.so";
    REQUIRE(Compiler::dependency_contribution_refusal(manifest, contribution).has_value());

    contribution.link[0].file = "libssl.so.3";
    REQUIRE_FALSE(Compiler::dependency_contribution_refusal(manifest, contribution).has_value());
}

TEST_CASE("a dependency's flag that loads a plugin refuses the build", "[trust]")
{
    ScopedProject project("plugin_flag");
    write_project(project.root(),
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: flag \"-fplugin=/tmp/never-loaded.so\"]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("Dependency Refused") != std::string::npos);
    REQUIRE(built.output.find("-fplugin=/tmp/never-loaded.so") != std::string::npos);
}

TEST_CASE("a dependency's ordinary flags still build", "[trust]")
{
    ScopedProject project("ordinary_flags");
    write_project(project.root(),
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: flag \"-Wno-unused-function\"]\n"
        "#[cc: define { DEP_LEVEL: 2 }]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code == 0);
}

TEST_CASE("a dependency's include flag is still refused", "[trust]")
{
    ScopedProject project("include_flag");
    write_project(project.root(),
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: flag \"-I/etc\"]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("Dependency Refused") != std::string::npos);
    REQUIRE(built.output.find("-I/etc") != std::string::npos);
}

TEST_CASE("a root's own flags are its business", "[trust]")
{
    ScopedProject project("root_flags");
    write_project(project.root(), "");

    write_file(project.root() / "app" / "inc" / "shim.h", "#define SHIM 1\n");
    write_file(project.root() / "app" / "c" / "a.c", "#include \"shim.h\"\nint app_c(void) { return SHIM; }\n");
    write_file(project.root() / "app" / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"*.eco\"]\n"
        "#[depends: \"../dep\"]\n"
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: flag \"-I" + (project.root() / "app" / "inc").generic_string() + "\"]\n");

    REQUIRE(project.echoc("build -o out", project.root() / "app").exit_code == 0);
}

TEST_CASE("a dependency's C source that is a symlink out of its tree is refused", "[trust]")
{
    ScopedProject project("c_symlink_out");
    write_file(project.root() / "secret.c", "int steal(void) { return 1; }\n");
    write_project(project.root(), "#[cc: sources \"c/*.c\"]\n");
    fs::remove(project.root() / "dep" / "c" / "h.c");

    std::error_code link_error;
    fs::create_symlink(project.root() / "secret.c", project.root() / "dep" / "c" / "h.c", link_error);
    if (link_error) {
        SKIP("this account cannot create symlinks");
    }

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("outside that module's directory") != std::string::npos);
}

TEST_CASE("a dependency's source outside its own directory refuses the build", "[trust]")
{
    ScopedProject project("outside_source");
    write_project(project.root(), "#[sources: \"../app/main.eco\"]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("outside that module's directory") != std::string::npos);
}

TEST_CASE("a dependency's include outside its own directory refuses the build", "[trust]")
{
    ScopedProject project("outside_include");
    fs::create_directories(project.root() / "outside");
    write_project(project.root(),
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: include \"../outside\"]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("Dependency Refused") != std::string::npos);
    REQUIRE(built.output.find("outside that module's directory") != std::string::npos);
}

TEST_CASE("a dependency's include inside its tree still builds", "[trust]")
{
    ScopedProject project("inside_include");
    write_project(project.root(),
        "#[cc: sources \"c/*.c\"]\n"
        "#[cc: include \"c\"]\n");

    REQUIRE(project.echoc("build -o out", project.root() / "app").exit_code == 0);
}

TEST_CASE("a dependency's search path outside its own directory refuses the build", "[trust]")
{
    ScopedProject project("outside_search");
    fs::create_directories(project.root() / "outside");
    write_project(project.root(), "#[link: search \"../outside\"]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("Dependency Refused") != std::string::npos);
    REQUIRE(built.output.find("outside that module's directory") != std::string::npos);
}

TEST_CASE("a dependency's object outside its own directory refuses the build", "[trust]")
{
    ScopedProject project("outside_object");
    write_file(project.root() / "outside" / "evil.o", "not-an-object\n");
    write_project(project.root(), "#[link: object \"../outside/evil.o\"]\n");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("Dependency Refused") != std::string::npos);
    REQUIRE(built.output.find("outside that module's directory") != std::string::npos);
}

TEST_CASE("a module name is one path component", "[trust]")
{
    ScopedProject project("module_name");

    write_file(project.root() / "lib" / "module.eco",
        "#[module: \"../escape\"]\n"
        "#[sources: \"*.eco\"]\n");
    write_file(project.root() / "lib" / "lib.eco", "public function f() : int32 { return 1; }\n");

    const ProcessResult built = project.echoc("build -o out -m " + quoted(project.root() / "lib"), project.root());

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("is not a usable module name") != std::string::npos);
}

TEST_CASE("a dependency's build_dir outside its tree is not honoured", "[trust]")
{
    ScopedProject project("dependency_build_dir");
    write_project(project.root(), "#[build_dir: \"../elsewhere\"]\n");

    REQUIRE(project.echoc("build -o out", project.root() / "app").exit_code == 0);

    std::error_code ec;
    REQUIRE_FALSE(fs::exists(project.root() / "elsewhere", ec));
    REQUIRE(fs::exists(project.root() / "dep" / "ecobuild" / "CACHEDIR.TAG", ec));
}

#if !defined(_WIN32)

TEST_CASE("a dependency's build directory that arrived as a link is refused", "[trust]")
{
    ScopedProject project("linked_build_dir");
    write_project(project.root(), "");

    fs::create_directories(project.root() / "victim");
    fs::create_directory_symlink(project.root() / "victim", project.root() / "dep" / "ecobuild");

    const ProcessResult built = project.echoc("build -o out", project.root() / "app");

    REQUIRE(built.exit_code != 0);
    REQUIRE(built.output.find("is not a directory") != std::string::npos);
    REQUIRE(fs::is_empty(project.root() / "victim"));
}

TEST_CASE("a planted link in a build directory is replaced, never written through", "[trust]")
{
    ScopedProject project("linked_marker");
    write_project(project.root(), "");

    write_file(project.root() / "victim.txt", "precious\n");
    fs::create_directories(project.root() / "dep" / "ecobuild");
    fs::create_symlink(project.root() / "victim.txt", project.root() / "dep" / "ecobuild" / "CACHEDIR.TAG");

    REQUIRE(project.echoc("build -o out", project.root() / "app").exit_code == 0);

    REQUIRE(read_text(project.root() / "victim.txt") == "precious\n");
    REQUIRE_FALSE(fs::is_symlink(project.root() / "dep" / "ecobuild" / "CACHEDIR.TAG"));
}

#endif

TEST_CASE("an object this user's echoc did not seal is rebuilt", "[trust][cache]")
{
    ScopedProject project("unsealed_object");
    write_project(project.root(), "");

    const std::vector<std::pair<std::string, std::string>> mine = {
        { "XDG_CACHE_HOME", (project.root() / "cache-mine").string() } };
    const std::vector<std::pair<std::string, std::string>> theirs = {
        { "XDG_CACHE_HOME", (project.root() / "cache-theirs").string() } };

    // built under one user's key: a second build under the same key reuses the object
    REQUIRE(project.echoc("build -o out", project.root() / "app", mine).exit_code == 0);
    write_file(project.root() / "app" / "main.eco", "echo twice(20);\n");

    const ProcessResult again = project.echoc("build -o out --explain cache", project.root() / "app", mine);
    REQUIRE(again.exit_code == 0);
    REQUIRE(cache_line_for(again.output, "dep").find("hit") != std::string::npos);

    // under another key, the very same bytes are something this user's echoc never built
    write_file(project.root() / "app" / "main.eco", "echo twice(19);\n");
    const ProcessResult foreign = project.echoc("build -o out --explain cache", project.root() / "app", theirs);
    REQUIRE(foreign.exit_code == 0);
    REQUIRE(cache_line_for(foreign.output, "dep").find("did not seal it") != std::string::npos);
}

TEST_CASE("a changed byte in a kept object is a rebuild", "[trust][cache]")
{
    ScopedProject project("flipped_byte");
    write_project(project.root(), "");

    const std::vector<std::pair<std::string, std::string>> env = {
        { "XDG_CACHE_HOME", (project.root() / "cache").string() } };

    REQUIRE(project.echoc("build -o out", project.root() / "app", env).exit_code == 0);

    const fs::path object = only_object_in(project.root() / "dep" / "ecobuild");
    REQUIRE_FALSE(object.empty());

    {
        std::fstream file(object, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(64);
        file.put('\x5a');
    }

    write_file(project.root() / "app" / "main.eco", "echo twice(20);\n");
    const ProcessResult rebuilt = project.echoc("build -o out --explain cache", project.root() / "app", env);

    REQUIRE(rebuilt.exit_code == 0);
    REQUIRE(cache_line_for(rebuilt.output, "dep").find("did not seal it") != std::string::npos);
}

TEST_CASE("a stamp and a binary that arrived together are rebuilt, not trusted", "[trust][cache]")
{
    ScopedProject project("shipped_binary");
    write_project(project.root(), "");
    write_file(project.root() / "app" / "module.eco",
        "#[module: \"app\"]\n"
        "#[sources: \"*.eco\"]\n"
        "#[depends: \"../dep\"]\n"
        "#[target: exe { name: \"app\", entry: \"main.eco\" }]\n");

    const std::vector<std::pair<std::string, std::string>> theirs = {
        { "XDG_CACHE_HOME", (project.root() / "cache-theirs").string() } };
    const std::vector<std::pair<std::string, std::string>> mine = {
        { "XDG_CACHE_HOME", (project.root() / "cache-mine").string() } };

    // what somebody else's machine left in the tree: a binary, and the stamp vouching for it
    REQUIRE(project.echoc("build", project.root() / "app", theirs).exit_code == 0);

    const ProcessResult built = project.echoc("build --explain cache", project.root() / "app", mine);

    REQUIRE(built.exit_code == 0);
    REQUIRE(built.output.find("up to date") == std::string::npos);
}

TEST_CASE("lsp refuses a dependency source outside its tree", "[trust][lsp]")
{
    ScopedProject project("lsp_outside_source");
    write_project(project.root(), "#[sources: \"../app/main.eco\"]\n");

    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    session.set_workspace_root(project.root() / "app");
    session.did_open(project.root() / "app" / "main.eco", 1, "echo twice(21);\n");
    session.rebuild();

    REQUIRE(session.parse_failure().has_value());
    REQUIRE(session.parse_failure()->title == "Dependency Refused");
    REQUIRE(session.parse_failure()->message.find("outside that module's directory") != std::string::npos);
}

#if !defined(_WIN32)

TEST_CASE("a world-readable cache key is reused, not rotated", "[trust][cache]")
{
    ScopedProject project("world_readable_key");
    write_project(project.root(), "");

    const fs::path cache = project.root() / "cache";
    const std::vector<std::pair<std::string, std::string>> env = {
        { "XDG_CACHE_HOME", cache.string() } };

    REQUIRE(project.echoc("build -o out", project.root() / "app", env).exit_code == 0);

    const fs::path key = cache / "echo" / "cache-key";
    REQUIRE(fs::exists(key));
    const std::string before = read_text(key);

    fs::permissions(
        key,
        fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all,
        fs::perm_options::replace);

    write_file(project.root() / "app" / "main.eco", "echo twice(20);\n");
    const ProcessResult again =
        project.echoc("build -o out --explain cache", project.root() / "app", env);

    REQUIRE(again.exit_code == 0);
    REQUIRE(cache_line_for(again.output, "dep").find("hit") != std::string::npos);
    REQUIRE(read_text(key) == before);
}

#endif
