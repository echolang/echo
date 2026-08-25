#include <catch2/catch_test_macros.hpp>

#include <AST/ASTFile.h>
#include <Compiler/Lsp/LspPosition.h>

TEST_CASE("get_content_of_line is 1-based and empty for line 0", "[lsp]")
{
    AST::File file("/tmp/line-index.eco");
    file.set_content("alpha\nbeta\n");

    REQUIRE(file.get_content_of_line(0) == "");
    REQUIRE(file.get_content_of_line(1) == "alpha");
    REQUIRE(file.get_content_of_line(2) == "beta");
    REQUIRE(file.get_content_of_line(3) == "");
    REQUIRE(file.line_count() == 3);
}

TEST_CASE("UTF-16 columns count an umlaut as one unit and an emoji as two", "[lsp]")
{
    // café = c a f é  - é is two UTF-8 bytes, one UTF-16 unit
    REQUIRE(Compiler::Lsp::utf16_column_of("café", 0) == 0);
    REQUIRE(Compiler::Lsp::utf16_column_of("café", 3) == 3);
    REQUIRE(Compiler::Lsp::utf16_column_of("café", 5) == 4);

    REQUIRE(Compiler::Lsp::byte_column_of_utf16("café", 4) == 5);

    // 😀 is four UTF-8 bytes, one scalar, two UTF-16 units
    const std::string emoji = "a😀b";
    REQUIRE(emoji.size() == 6);
    REQUIRE(Compiler::Lsp::utf16_column_of(emoji, 1) == 1);
    REQUIRE(Compiler::Lsp::utf16_column_of(emoji, 5) == 3);
    REQUIRE(Compiler::Lsp::byte_column_of_utf16(emoji, 3) == 5);
}

TEST_CASE("an Echo location converts to a 0-based LSP position", "[lsp]")
{
    AST::File file("/tmp/pos.eco");
    file.set_content("int32 $n = 1;\n");

    const Compiler::Lsp::Position pos = Compiler::Lsp::lsp_position_of(
        file, AST::Location{ 1, 7 }, false);

    REQUIRE(pos.line == 0);
    REQUIRE(pos.character == 6);
}

TEST_CASE("utf-8 encoding leaves the byte column alone", "[lsp]")
{
    AST::File file("/tmp/utf8.eco");
    file.set_content("café\n");

    const Compiler::Lsp::Position pos = Compiler::Lsp::lsp_position_of(
        file, AST::Location{ 1, 5 }, true);

    REQUIRE(pos.line == 0);
    REQUIRE(pos.character == 4);
}
