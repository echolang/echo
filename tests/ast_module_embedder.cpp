#include <catch2/catch_test_macros.hpp>

#include <AST/ASTModule.h>
#include <AST/ASTModuleEmbedder.h>
#include <Compiler/LinkRequirement.h>
#include <Compiler/TargetFacts.h>
#include <Parser/ManifestParser.h>
#include <eco.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "subprocess.h"
#include "test_lane.h"

// AST::embedded_source_path is the sole owner of the path written into
// stdlib_embedded.h. the Windows release regenerates that header on the runner,
// and `path.string()` there is `D:\a\echo\echo\stdlib\core\ordered_map.eco` -
// a C string whose `\o` and `\u` are compile errors, not a path

namespace fs = std::filesystem;

namespace
{

std::string with_backslashes(std::string path)
{
    for (char &c : path) {
        if (c == '/') {
            c = '\\';
        }
    }
    return path;
}

std::string read_file(const fs::path &path)
{
    std::ifstream in(path);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

};

TEST_CASE("a stdlib source is rewritten to a virtual generic path", "[embedder]")
{
    const fs::path native = fs::path(STDLIB_SOURCE_DIR) / "core" / "ordered_map.eco";
    REQUIRE(AST::embedded_source_path(native) == "stdlib:/core/ordered_map.eco");
}

TEST_CASE("native backslashes still rewrite to stdlib: with forward slashes", "[embedder]")
{
    // the windows bug, host-independent: cmake's STDLIB_SOURCE_DIR uses `/`,
    // filesystem::path::string() on Windows uses `\`, and the prefix match
    // failed so the raw native path was written into a C string
    const std::string native = with_backslashes(
        (fs::path(STDLIB_SOURCE_DIR) / "core" / "utf8.eco").generic_string());
    REQUIRE(AST::embedded_source_path(native) == "stdlib:/core/utf8.eco");
}

TEST_CASE("write_embedded_module emits a generic add_file path as a C string", "[embedder]")
{
    AST::Module module("stdlib", 0);
    auto &file = module.add_file(fs::path(STDLIB_SOURCE_DIR) / "core" / "ordered_map.eco");
    file.set_content("function f() {}\n");

    const fs::path tmp_dir = fs::path(EchoTests::e2e_tmp_dir()) / "embedder";
    fs::create_directories(tmp_dir);
    const fs::path tmp = tmp_dir / "stdlib_embedded.h";
    AST::write_embedded_module(module, tmp.string());

    const std::string contents = read_file(tmp);
    REQUIRE(contents.find("module.add_file(\"stdlib:/core/ordered_map.eco\")") != std::string::npos);
    REQUIRE(contents.find('\\') == std::string::npos);
}

TEST_CASE("stdlib_embedded.h is included from exactly one translation unit", "[embedder]")
{
    // the header embeds every stdlib file as a static table. a second include
    // is a second copy of those bytes on the ECO_EMBED_STDLIB link
    const fs::path src = fs::path(STDLIB_SOURCE_DIR).parent_path() / "src";
    std::vector<std::string> holders;

    for (const auto &entry : fs::recursive_directory_iterator(src)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }

        const std::string contents = read_file(entry.path());
        if (contents.find("#include \"stdlib_embedded.h\"") != std::string::npos) {
            holders.push_back(entry.path().lexically_relative(src).generic_string());
        }
    }

    REQUIRE(holders == std::vector<std::string>{ "Compiler/ModuleCache.cpp" });
}

TEST_CASE("collect_stdlib_files includes module.eco and gated sources", "[embedder]")
{
    const auto files = AST::collect_stdlib_files(STDLIB_SOURCE_DIR);
    REQUIRE_FALSE(files.empty());

    bool saw_manifest = false;
    bool saw_thread = false;
    bool saw_dynlib = false;

    for (const auto &[path, content] : files) {
        if (path == "module.eco") {
            saw_manifest = true;
        }
        if (path.find("std/thread/") == 0) {
            saw_thread = true;
        }
        if (path.find("std/dynlib/") == 0) {
            saw_dynlib = true;
        }
        REQUIRE(path.find("build/") != 0);
        REQUIRE(path.find("sketches/") != 0);
        (void)content;
    }

    REQUIRE(saw_manifest);
    REQUIRE(saw_thread);
    REQUIRE(saw_dynlib);
}

TEST_CASE("stdlib module.eco drops thread and dynlib on wasi", "[embedder][wasi]")
{
    std::string error;
    Compiler::TargetFacts facts;
    REQUIRE(Compiler::TargetFacts::resolve("wasi", "wasm32", {}, facts, error));
    REQUIRE(error.empty());

    const auto files = AST::collect_stdlib_files(STDLIB_SOURCE_DIR);
    std::vector<std::string> pool;
    pool.reserve(files.size());
    for (const auto &[path, _] : files) {
        pool.push_back(path);
    }

    Parser::ManifestScratch scratch(facts);
    scratch.virtual_source_pool = pool;

    Parser::ModuleManifest manifest;
    REQUIRE(Parser::read_module_manifest(
        std::filesystem::path(STDLIB_SOURCE_DIR) / "module.eco", scratch, manifest));

    for (const auto &source : manifest.sources) {
        const std::string spelled = source.generic_string();
        REQUIRE(spelled.find("std/thread/") == std::string::npos);
        REQUIRE(spelled.find("std/dynlib/") == std::string::npos);
    }

    for (const auto &req : manifest.link) {
        REQUIRE(req.value != "pthread");
        REQUIRE(req.value != "dl");
    }
}

TEST_CASE("write_embedded_stdlib emits a file table and module.eco", "[embedder]")
{
    const fs::path tmp_dir = fs::path(EchoTests::e2e_tmp_dir()) / "embedder_stdlib";
    fs::create_directories(tmp_dir);
    const fs::path tmp = tmp_dir / "stdlib_embedded.h";
    AST::write_embedded_stdlib(STDLIB_SOURCE_DIR, tmp.string());

    const std::string contents = read_file(tmp);
    REQUIRE(contents.find("content_of") != std::string::npos);
    REQUIRE(contents.find("\"module.eco\"") != std::string::npos);
    REQUIRE(contents.find("\"std/thread/thread.eco\"") != std::string::npos);
    REQUIRE(contents.find("load_stdlib_module") == std::string::npos);
    REQUIRE(contents.find("#include \"AST/") == std::string::npos);
    REQUIRE(contents.find("cannot open") == std::string::npos);
}

#if ECO_USE_EMBEDDED_STDLIB
TEST_CASE("an embedded-stdlib echoc compiles a hello world", "[embedder]")
{
    EchoTests::ScopedProject project("embedder", "hello_embedded");
    EchoTests::write_file(project.root() / "hello.eco", "echo 1;\n");

    const EchoTests::ProcessResult ran = project.echoc("run hello.eco");
    INFO(ran.output);
    REQUIRE(ran.exit_code == 0);
    REQUIRE(ran.output.find("1") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(project.root() / "stdlib:"));
}
#endif
