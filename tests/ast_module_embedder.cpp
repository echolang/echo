#include <catch2/catch_test_macros.hpp>

#include <AST/ASTModule.h>
#include <AST/ASTModuleEmbedder.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

    const fs::path tmp_dir = fs::path(ECO_E2E_TMP_DIR) / "embedder";
    fs::create_directories(tmp_dir);
    const fs::path tmp = tmp_dir / "stdlib_embedded.h";
    AST::write_embedded_module(module, tmp.string());

    const std::string contents = read_file(tmp);
    REQUIRE(contents.find("module.add_file(\"stdlib:/core/ordered_map.eco\")") != std::string::npos);
    REQUIRE(contents.find('\\') == std::string::npos);
}
