#include <catch2/catch_test_macros.hpp>

#include <AST/ASTFile.h>
#include <Compiler/Lsp/LspLiveText.h>

#include <memory>
#include <string>

namespace
{
    std::unique_ptr<AST::File> compiled_file(const std::string &content)
    {
        auto file = std::make_unique<AST::File>("/tmp/livetext.eco");
        file->set_content(content);
        return file;
    }
};

TEST_CASE("live text splits lines the way AST::File does", "[lsp]")
{
    const Compiler::Lsp::LiveText live("alpha\r\nbeta\n");

    REQUIRE(live.line_count() == 3);
    REQUIRE(live.line(0) == "");
    REQUIRE(live.line(1) == "alpha\r");
    REQUIRE(live.line(2) == "beta");
    REQUIRE(live.line(3) == "");
    REQUIRE(live.line(4) == "");

    REQUIRE(live.offset_of(AST::Location{ 2, 1 }) == 7);
    REQUIRE(live.location_of(7).line == 2);
    REQUIRE(live.location_of(7).column == 1);
    REQUIRE(live.location_of(9).column == 3);
}

TEST_CASE("identical text maps every line to itself", "[lsp]")
{
    const auto file = compiled_file("a\nb\nc\n");
    const Compiler::Lsp::LineMap map = Compiler::Lsp::LineMap::build(*file, Compiler::Lsp::LiveText("a\nb\nc\n"));

    REQUIRE(map.identical());
    REQUIRE(map.to_snapshot(2) == 2u);
    REQUIRE(map.to_live(3) == 3u);
}

TEST_CASE("an inserted line maps the lines above and the shifted lines below", "[lsp]")
{
    const auto file = compiled_file("one\ntwo\nthree\n");
    const Compiler::Lsp::LineMap map = Compiler::Lsp::LineMap::build(
        *file, Compiler::Lsp::LiveText("one\nnew\ntwo\nthree\n"));

    REQUIRE_FALSE(map.identical());
    REQUIRE(map.to_snapshot(1) == 1u);
    REQUIRE_FALSE(map.to_snapshot(2).has_value());
    REQUIRE(map.to_snapshot(3) == 2u);
    REQUIRE(map.to_snapshot(4) == 3u);

    REQUIRE(map.to_live(2) == 3u);
    REQUIRE(map.to_live(3) == 4u);
}

TEST_CASE("an edited line maps to nothing and its neighbours stay put", "[lsp]")
{
    const auto file = compiled_file("one\ntwo\nthree\n");
    const Compiler::Lsp::LineMap map = Compiler::Lsp::LineMap::build(
        *file, Compiler::Lsp::LiveText("one\ntwo $p->\nthree\n"));

    REQUIRE(map.to_snapshot(1) == 1u);
    REQUIRE_FALSE(map.to_snapshot(2).has_value());
    REQUIRE(map.to_snapshot(3) == 3u);
    REQUIRE_FALSE(map.to_live(2).has_value());
}

TEST_CASE("deleted lines shift what follows them up", "[lsp]")
{
    const auto file = compiled_file("one\ntwo\nthree\nfour\n");
    const Compiler::Lsp::LineMap map = Compiler::Lsp::LineMap::build(
        *file, Compiler::Lsp::LiveText("one\nfour\n"));

    REQUIRE(map.to_snapshot(1) == 1u);
    REQUIRE(map.to_snapshot(2) == 4u);
    REQUIRE_FALSE(map.to_live(2).has_value());
    REQUIRE_FALSE(map.to_live(3).has_value());
    REQUIRE(map.to_live(4) == 2u);
}

TEST_CASE("a CRLF file lines up with itself", "[lsp]")
{
    const auto file = compiled_file("one\r\ntwo\r\n");
    const Compiler::Lsp::LineMap map = Compiler::Lsp::LineMap::build(
        *file, Compiler::Lsp::LiveText("zero\r\none\r\ntwo\r\n"));

    REQUIRE_FALSE(map.to_snapshot(1).has_value());
    REQUIRE(map.to_snapshot(2) == 1u);
    REQUIRE(map.to_snapshot(3) == 2u);
}

TEST_CASE("UTF-16 columns are measured on the live line", "[lsp]")
{
    // the snapshot still has the old line, so only the live line can say the 'x' is at byte 7
    const Compiler::Lsp::LiveText live("$é = 1; $x\n");

    const AST::Location location = Compiler::Lsp::live_location_of(live, Compiler::Lsp::Position{ 0, 8 }, false);
    REQUIRE(location.line == 1);
    REQUIRE(location.column == 10);

    const Compiler::Lsp::Position back = Compiler::Lsp::live_position_of(live, location, false);
    REQUIRE(back.line == 0);
    REQUIRE(back.character == 8);
}
