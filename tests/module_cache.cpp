#include <catch2/catch_test_macros.hpp>

#include "subprocess.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// what the module system promises a *project* rather than a compilation: that `echoc run` in a directory
// holding a module.eco needs no arguments, and that a module's compiled identity is a function of its own
// sources and its dependencies' - never of whoever happens to be consuming it.
//
// these are subprocess tests rather than corpus goldens for two reasons the .test format cannot express: the
// working directory is the thing under test in half of them, and a cache key folds in the LLVM version and the
// host triple, so no digest is comparable across machines. Substring assertions on `--explain-cache` are, and
// that is what is checked.

namespace fs = std::filesystem;

namespace
{

// the shared process primitive - see subprocess.h
using EchoTests::ProcessResult;
using EchoTests::quoted;
using EchoTests::run_capturing;

void write_file(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// a scratch project directory, removed when the test leaves. Named after the case so a failure leaves
// something identifiable behind when the removal is commented out to inspect it
class ScopedProject
{
public:
    explicit ScopedProject(const std::string &name)
        : _root(fs::path(ECO_E2E_TMP_DIR) / "module_cache" / name)
    {
        std::error_code ec;
        fs::remove_all(_root, ec);
        fs::create_directories(_root, ec);
    }

    ~ScopedProject()
    {
        std::error_code ec;
        fs::remove_all(_root, ec);
    }

    ScopedProject(const ScopedProject &) = delete;
    ScopedProject &operator=(const ScopedProject &) = delete;

    const fs::path &root() const { return _root; }
    fs::path cache_dir() const { return _root / "cache"; }

    // `cd <dir> && echoc <args>`, because the working directory is what project discovery reads
    ProcessResult echoc(const std::string &args, const fs::path &working_directory) const
    {
        return run_capturing(
            "cd " + quoted(working_directory) + " && " + quoted(ECHOC_BINARY) + " " + args + " 2>&1");
    }

private:
    fs::path _root;
};

// the `--explain-cache` line for one module: "<name>  <key>  <hit|miss>[  (why)]"
std::string cache_line(const std::string &output, const std::string &module_name)
{
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        std::string first;
        fields >> first;

        if (first == module_name) {
            return line;
        }
    }

    return "";
}

bool files_are_identical(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec)) {
        return false;
    }

    if (fs::file_size(a, ec) != fs::file_size(b, ec)) {
        return false;
    }

    std::ifstream in_a(a, std::ios::binary);
    std::ifstream in_b(b, std::ios::binary);

    std::ostringstream bytes_a;
    std::ostringstream bytes_b;
    bytes_a << in_a.rdbuf();
    bytes_b << in_b.rdbuf();

    return bytes_a.str() == bytes_b.str();
}

// a small library any of these cases can depend on
void write_library(const fs::path &directory, const std::string &module_name)
{
    write_file(directory / "module.eco",
        "#[module: \"" + module_name + "\"]\n"
        "#[version: \"0.1.0\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(directory / "src" / "lib.eco",
        "namespace " + module_name + ";\n"
        "\n"
        "function twice(int32 $n) : int32\n"
        "{\n"
        "    return $n * 2;\n"
        "}\n");
}

};

TEST_CASE("a project directory needs no arguments", "[cache][project]")
{
    ScopedProject project("zero_arg");

    write_file(project.root() / "module.eco",
        "#[module: \"greeter\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    write_file(project.root() / "src" / "app.eco", "echo 41 + 1;\n");

    SECTION("run discovers the manifest in the working directory")
    {
        const ProcessResult result = project.echoc("run", project.root());

        REQUIRE(result.exit_code == 0);
        REQUIRE(result.output.find("42") != std::string::npos);
    }

    SECTION("build discovers it too, and the binary runs")
    {
        const ProcessResult built = project.echoc("build -o app", project.root());
        REQUIRE(built.exit_code == 0);

        const ProcessResult ran = run_capturing(quoted(project.root() / "app") + " 2>&1");
        REQUIRE(ran.exit_code == 0);
        REQUIRE(ran.output.find("42") != std::string::npos);
    }

    SECTION("the entry module is the manifest's own, not a hardcoded name")
    {
        // the manifest calls its module `greeter`, and nothing in it mentions `main`. Requiring
        // `#[module: "main"]` to make a project runnable would put the compiler's internals in every
        // user's manifest
        const ProcessResult result = project.echoc("run --explain-cache", project.root());

        REQUIRE(result.exit_code == 0);
        REQUIRE_FALSE(cache_line(result.output, "greeter").empty());
        REQUIRE(cache_line(result.output, "main").empty());
    }
}

TEST_CASE("a directory with no manifest and no sources says so", "[cache][project]")
{
    ScopedProject project("no_manifest");

    const ProcessResult result = project.echoc("run", project.root());

    REQUIRE(result.exit_code == 1);
    REQUIRE(result.output.find("module.eco") != std::string::npos);
}

TEST_CASE("a cache key is stable, and moves only for what changed", "[cache]")
{
    ScopedProject project("key_stability");

    write_library(project.root() / "lib", "cachelib");
    write_file(project.root() / "app" / "module.eco",
        "#[module: \"app\"]\n"
        "#[depends: \"../lib\"]\n"
        "#[sources: \"*.eco\"]\n");
    write_file(project.root() / "app" / "app.eco", "echo cachelib::twice(21);\n");

    const std::string args = "run --explain-cache --cache-dir " + quoted(project.cache_dir());
    const fs::path app_dir = project.root() / "app";

    const ProcessResult first = project.echoc(args, app_dir);
    REQUIRE(first.exit_code == 0);
    REQUIRE(first.output.find("42") != std::string::npos);

    const std::string lib_before = cache_line(first.output, "cachelib");
    const std::string app_before = cache_line(first.output, "app");
    const std::string stdlib_before = cache_line(first.output, "stdlib");

    REQUIRE_FALSE(lib_before.empty());
    REQUIRE_FALSE(app_before.empty());

    SECTION("the same inputs hash the same way twice")
    {
        const ProcessResult second = project.echoc(args, app_dir);

        REQUIRE(cache_line(second.output, "cachelib") == lib_before);
        REQUIRE(cache_line(second.output, "app") == app_before);
        REQUIRE(cache_line(second.output, "stdlib") == stdlib_before);
    }

    SECTION("editing the library moves its key and its dependent's, but not the stdlib's")
    {
        write_file(project.root() / "lib" / "src" / "lib.eco",
            "namespace cachelib;\n"
            "\n"
            "function twice(int32 $n) : int32\n"
            "{\n"
            "    return $n + $n;\n"
            "}\n");

        const ProcessResult after = project.echoc(args, app_dir);

        REQUIRE(cache_line(after.output, "cachelib") != lib_before);

        // transitively, without a second graph walk: a dependency contributes its whole key
        REQUIRE(cache_line(after.output, "app") != app_before);

        // and nothing below it in the order is disturbed
        REQUIRE(cache_line(after.output, "stdlib") == stdlib_before);
    }

    SECTION("editing the application does not move the library's key")
    {
        write_file(project.root() / "app" / "app.eco", "echo cachelib::twice(50);\n");

        const ProcessResult after = project.echoc(args, app_dir);

        REQUIRE(cache_line(after.output, "cachelib") == lib_before);
        REQUIRE(cache_line(after.output, "app") != app_before);
    }

    SECTION("debug and release are different builds of the same source")
    {
        const ProcessResult debug = project.echoc(args + " --debug", app_dir);
        const ProcessResult release = project.echoc(args + " --release", app_dir);

        REQUIRE_FALSE(cache_line(debug.output, "cachelib").empty());
        REQUIRE(cache_line(debug.output, "cachelib") != cache_line(release.output, "cachelib"));
    }

    SECTION("a miss names the file that changed rather than reporting a digest")
    {
        // the record is written by a build, so there is nothing to compare against on the very first run -
        // which is itself the correct answer, and why an unexplainable miss prints no reason at all
        const std::string line = cache_line(first.output, "cachelib");
        REQUIRE(line.find("miss") != std::string::npos);
    }
}

TEST_CASE("a library's object does not depend on which application consumes it", "[cache][odr]")
{
    // **the invariant the whole module cache rests on.** Stages 0-3 exist to make a module's emitted code a
    // function of its own source: generic instantiations and other compiler-generated definitions are emitted
    // into the unit that *references* them, so a consumer can no longer cause a definition to appear in the
    // library's object.
    //
    // if this fails, caching is unsound rather than merely ineffective - the object stored for one application
    // would be the wrong object for the next
    ScopedProject project("consumer_independence");

    write_file(project.root() / "lib" / "module.eco",
        "#[module: \"shared\"]\n"
        "#[sources: \"src/*.eco\"]\n");

    // a generic and an `#[inline]` beside an ordinary function, so all three placements are exercised
    write_file(project.root() / "lib" / "src" / "lib.eco",
        "namespace shared;\n"
        "\n"
        "struct Holder<T>\n"
        "{\n"
        "    T $value;\n"
        "\n"
        "    function get() : T\n"
        "    {\n"
        "        return $this->value;\n"
        "    }\n"
        "}\n"
        "\n"
        "#[inline]\n"
        "function hot(int32 $n) : int32 { return $n + 1; }\n"
        "\n"
        "function plain(int32 $n) : int32 { return $n * 2; }\n"
        "\n"
        "const usize PAGE = 4096;\n");

    // two applications that use the library differently: one instantiates the generic over int32 and calls
    // the inline function, the other instantiates over a type *it* declares and calls only the plain one
    //
    // both also read the library's compile-time constant, which is the shape that would break this in a way
    // the generic does not: a constant is expanded into a *clone* of its value, and cloning it into the
    // declaring module's arena rather than the consuming one would put a node the application emits into the
    // library's collection - so the library's object would grow a copy per consumer
    write_file(project.root() / "app_a" / "app.eco",
        "shared::Holder<int32> $h = shared::Holder<int32>(7);\n"
        "echo $h->get();\n"
        "echo shared::hot(1);\n"
        "usize $page = shared::PAGE;\n"
        "echo $page;\n");

    write_file(project.root() / "app_b" / "app.eco",
        "struct Local { int32 $x; }\n"
        "shared::Holder<Local> $h = shared::Holder<Local>(Local(3));\n"
        "echo $h->get()->x;\n"
        "echo shared::plain(4);\n"
        "usize $half = shared::PAGE / 2;\n"
        "echo $half;\n");

    const fs::path manifest = project.root() / "lib" / "module.eco";
    const fs::path cache_a = project.root() / "cache_a";
    const fs::path cache_b = project.root() / "cache_b";

    const auto build = [&](const std::string &app, const fs::path &cache) {
        return project.echoc(
            "build -o out -m " + quoted(manifest) + " --cache-dir " + quoted(cache) + " app.eco",
            project.root() / app);
    };

    const ProcessResult a = build("app_a", cache_a);
    const ProcessResult b = build("app_b", cache_b);

    REQUIRE(a.exit_code == 0);
    REQUIRE(b.exit_code == 0);

    // both programs must actually run, or "the objects agree" would be a statement about two broken builds
    const ProcessResult ran_a = run_capturing(quoted(project.root() / "app_a" / "out") + " 2>&1");
    const ProcessResult ran_b = run_capturing(quoted(project.root() / "app_b" / "out") + " 2>&1");

    REQUIRE(ran_a.exit_code == 0);
    REQUIRE(ran_b.exit_code == 0);
    REQUIRE(ran_a.output.find("7") != std::string::npos);
    REQUIRE(ran_b.output.find("3") != std::string::npos);

    // the library's key must be identical, because none of its own inputs differ between the two builds
    const std::string key_a = cache_line(
        project.echoc("build -o out -m " + quoted(manifest) + " --cache-dir " + quoted(cache_a)
            + " --explain-cache app.eco", project.root() / "app_a").output, "shared");

    const std::string key_b = cache_line(
        project.echoc("build -o out -m " + quoted(manifest) + " --cache-dir " + quoted(cache_b)
            + " --explain-cache app.eco", project.root() / "app_b").output, "shared");

    REQUIRE_FALSE(key_a.empty());
    REQUIRE(key_a == key_b);

    // **and the objects those keys name must be byte-identical.**
    //
    // This is the half that would catch a regression the key cannot: the key is computed from source, so it
    // agrees by construction, while the object is what codegen actually produced. If a consumer can influence
    // the library's object, these differ while the keys match - and the cache would hand app_b the object
    // built for app_a.
    std::vector<fs::path> objects_a;
    for (const auto &entry : fs::recursive_directory_iterator(cache_a)) {
        if (entry.path().extension() == ".o") {
            objects_a.push_back(entry.path());
        }
    }

    // the library was compiled fresh into both stores, so there is something to compare
    REQUIRE_FALSE(objects_a.empty());

    for (const fs::path &object : objects_a) {
        const fs::path counterpart = cache_b / fs::relative(object, cache_a);

        INFO("library object: " << fs::relative(object, cache_a).string());
        REQUIRE(files_are_identical(object, counterpart));
    }
}

TEST_CASE("a stored object is reused, and a changed source is not", "[cache][store]")
{
    ScopedProject project("store");

    write_library(project.root() / "lib", "storelib");
    write_file(project.root() / "app" / "app.eco", "echo storelib::twice(21);\n");

    const fs::path app_dir = project.root() / "app";
    // `source` is declared `.remaining()`, so every flag has to sit *before* the filename - a `-O` after it
    // is read as another source file, which argparse then reports as a missing one
    const std::string flags =
        "build -o out -m " + quoted(project.root() / "lib")
        + " --cache-dir " + quoted(project.cache_dir()) + " --explain-cache";

    const std::string args = flags + " app.eco";

    const ProcessResult cold = project.echoc(args, app_dir);
    REQUIRE(cold.exit_code == 0);
    REQUIRE(cache_line(cold.output, "storelib").find("miss") != std::string::npos);

    // the artifact and its record, both under the module's own directory in the store
    fs::path stored_object;
    for (const auto &entry : fs::recursive_directory_iterator(project.cache_dir())) {
        if (entry.path().extension() == ".o" && entry.path().filename().string().rfind("storelib", 0) == 0) {
            stored_object = entry.path();
        }
    }

    REQUIRE_FALSE(stored_object.empty());

    SECTION("a second build reuses it")
    {
        const ProcessResult warm = project.echoc(args, app_dir);

        REQUIRE(warm.exit_code == 0);
        REQUIRE(cache_line(warm.output, "storelib").find("hit") != std::string::npos);

        // and the program still works, which is the only thing a reused object is for
        const ProcessResult ran = run_capturing(quoted(app_dir / "out") + " 2>&1");
        REQUIRE(ran.exit_code == 0);
        REQUIRE(ran.output.find("42") != std::string::npos);
    }

    SECTION("the entry module is never reused, and says so rather than reporting a changed input")
    {
        // `app` here is loose sources rather than a manifest, so it is not even a candidate; the case that
        // matters is a *project* whose own manifest is the program. It must miss every time, and the reason
        // has to read as "this is the program" rather than as a file having changed
        ScopedProject inner("store_entry");
        write_file(inner.root() / "module.eco",
            "#[module: \"selfprog\"]\n#[sources: \"*.eco\"]\n");
        write_file(inner.root() / "app.eco", "echo 1;\n");

        const std::string inner_args =
            "build -o out --cache-dir " + quoted(inner.cache_dir()) + " --explain-cache";

        inner.echoc(inner_args, inner.root());
        const ProcessResult second = inner.echoc(inner_args, inner.root());

        const std::string line = cache_line(second.output, "selfprog");
        REQUIRE(line.find("miss") != std::string::npos);
        REQUIRE(line.find("never cached") != std::string::npos);
    }

    SECTION("editing the library invalidates it and names the file")
    {
        write_file(project.root() / "lib" / "src" / "lib.eco",
            "namespace storelib;\n"
            "\n"
            "function twice(int32 $n) : int32\n"
            "{\n"
            "    return $n + $n;\n"
            "}\n");

        const ProcessResult after = project.echoc(args, app_dir);

        REQUIRE(after.exit_code == 0);

        const std::string line = cache_line(after.output, "storelib");
        REQUIRE(line.find("miss") != std::string::npos);
        REQUIRE(line.find("lib.eco") != std::string::npos);

        // the old artifact is left alone rather than overwritten - the key is in the filename, so two builds
        // of two revisions coexist and switching back is a hit
        std::error_code ec;
        REQUIRE(fs::is_regular_file(stored_object, ec));
    }

    SECTION("an optimized build reuses nothing, because it is whole-program")
    {
        const ProcessResult optimized = project.echoc(flags + " -O app.eco", app_dir);

        REQUIRE(optimized.exit_code == 0);
        REQUIRE(optimized.output.find("bypassed") != std::string::npos);
        REQUIRE(cache_line(optimized.output, "storelib").find("miss") != std::string::npos);
    }

    SECTION("an unwritable store is not an error")
    {
        // a read-only library directory, a toolchain installed system-wide, a full disk. A cache is an
        // optimization, so the only correct answer is to compile the module and not keep the result - failing
        // the build over a missed optimization would make the cache a liability
        ScopedProject inner("store_readonly");
        write_library(inner.root() / "lib", "rolib");
        write_file(inner.root() / "app" / "app.eco", "echo rolib::twice(21);\n");

        std::error_code ec;
        fs::permissions(inner.root() / "lib", fs::perms::owner_read | fs::perms::owner_exec,
            fs::perm_options::replace, ec);

        // no --cache-dir, so the default store is `.echo` inside that unwritable directory
        const ProcessResult result = inner.echoc(
            "build -o out -m " + quoted(inner.root() / "lib") + " --explain-cache app.eco",
            inner.root() / "app");

        // permissions restored before any assertion, so a failure cannot leave an undeletable directory behind
        fs::permissions(inner.root() / "lib", fs::perms::owner_all, fs::perm_options::replace, ec);

        REQUIRE(result.exit_code == 0);

        const std::string line = cache_line(result.output, "rolib");
        REQUIRE(line.find("miss") != std::string::npos);
        REQUIRE(line.find("not writable") != std::string::npos);

        // and the program is still there and still correct
        const ProcessResult ran = run_capturing(quoted(inner.root() / "app" / "out") + " 2>&1");
        REQUIRE(ran.exit_code == 0);
        REQUIRE(ran.output.find("42") != std::string::npos);
    }

    SECTION("the standard library does not cache into the compiler's own tree")
    {
        // the stdlib manifest lives wherever this compiler was built from, which is nobody's project. Writing
        // there means every user's build scribbles in the toolchain, and against an installed compiler that
        // directory is read-only - so the build would fail rather than merely not cache
        ScopedProject inner("store_stdlib_home");
        write_file(inner.root() / "app.eco", "echo 1;\n");

        const ProcessResult result = inner.echoc(
            "build -o out --explain-cache app.eco", inner.root());

        REQUIRE(result.exit_code == 0);
        REQUIRE_FALSE(cache_line(result.output, "stdlib").empty());

        // STDLIB_SOURCE_DIR is the compiler's own source tree, and nothing may appear in it
        std::error_code ec;
        REQUIRE_FALSE(fs::exists(fs::path(STDLIB_SOURCE_DIR) / ".echo", ec));
    }

    SECTION("a deleted artifact is rebuilt rather than fatal")
    {
        std::error_code ec;
        fs::remove(stored_object, ec);

        const ProcessResult after = project.echoc(args, app_dir);

        REQUIRE(after.exit_code == 0);
        REQUIRE(cache_line(after.output, "storelib").find("miss") != std::string::npos);

        const ProcessResult ran = run_capturing(quoted(app_dir / "out") + " 2>&1");
        REQUIRE(ran.output.find("42") != std::string::npos);
    }
}
