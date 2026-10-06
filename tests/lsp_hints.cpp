#include <catch2/catch_test_macros.hpp>

#include <AST/ASTFile.h>
#include <Compiler/DriverOptions.h>
#include <Compiler/Lsp/LspHints.h>
#include <Compiler/Lsp/LspSession.h>

#include <filesystem>
#include <string>

namespace
{
    Compiler::DriverOptions lsp_driver()
    {
        Compiler::DriverOptions driver;
        driver.subcommand = Compiler::Subcommand::t_lsp;
        driver.no_stdlib = true;
        return driver;
    }

    // the 1-based line and byte column of the `nth` occurrence of `needle`
    AST::Location location_of(const std::string &source, const std::string &needle, size_t nth = 0)
    {
        size_t at = source.find(needle);
        for (size_t i = 0; i < nth && at != std::string::npos; i++) {
            at = source.find(needle, at + 1);
        }
        REQUIRE(at != std::string::npos);

        const size_t newline = source.rfind('\n', at);
        const size_t line_begin = newline == std::string::npos ? 0 : newline + 1;
        uint32_t line = 1;
        for (size_t i = 0; i < at; i++) {
            if (source[i] == '\n') {
                line++;
            }
        }

        return AST::Location{ line, static_cast<uint32_t>(at - line_begin + 1) };
    }
};

TEST_CASE("highlight marks a variable's declaration and assignment as writes, its uses as reads", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-highlight.eco";
    const std::string source
        = "function main() : void {\n"
          "    int32 $n = 1;\n"
          "    $n = $n + 1;\n"
          "    echo $n;\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const auto highlights = Compiler::Lsp::document_highlights(
        *session.snapshot(), *file, location_of(source, "$n", 3));
    REQUIRE(highlights.size() == 4);

    int writes = 0;
    int reads = 0;
    for (const auto &highlight : highlights) {
        if (highlight.kind == Compiler::Lsp::HighlightKind::t_write) {
            writes++;
        }
        if (highlight.kind == Compiler::Lsp::HighlightKind::t_read) {
            reads++;
        }
    }

    REQUIRE(writes == 2);
    REQUIRE(reads == 2);
}

TEST_CASE("highlight stays inside the file it was asked of", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path lib = "/tmp/lsp-highlight-lib.eco";
    const std::filesystem::path app = "/tmp/lsp-highlight-app.eco";
    const std::string lib_source = "function twice(int32 $a) : int32 { return $a * 2; }\n";
    const std::string app_source = "function main() : void { echo twice(1); echo twice(2); }\n";
    session.did_open(lib, 1, lib_source);
    session.did_open(app, 1, app_source);
    session.rebuild();

    const AST::File *file = session.file_of(app);
    REQUIRE(file != nullptr);

    const auto highlights = Compiler::Lsp::document_highlights(
        *session.snapshot(), *file, location_of(app_source, "twice"));
    REQUIRE(highlights.size() == 2);
    for (const auto &highlight : highlights) {
        REQUIRE(highlight.range.file == file);
        REQUIRE(highlight.kind == Compiler::Lsp::HighlightKind::t_read);
    }
}

TEST_CASE("an inferred declaration gets its type as an inlay hint", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-inlay.eco";
    const std::string source
        = "function f(int32 $a) : float64 {\n"
          "    $x = 1;\n"
          "    int32 $y = 2;\n"
          "    $z = 1.5;\n"
          "    return $z;\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const auto hints = Compiler::Lsp::inlay_hints(*session.snapshot(), *file, 1, 100);
    REQUIRE(hints.size() == 2);

    REQUIRE(hints[0].position.line == 2);
    REQUIRE(hints[0].position.column == location_of(source, "$x").column);
    REQUIRE(hints[0].label == "int32");
    REQUIRE(hints[1].position.line == 4);
    REQUIRE(hints[1].label == "float64");

    // the range is honoured
    REQUIRE(Compiler::Lsp::inlay_hints(*session.snapshot(), *file, 3, 3).empty());
}

TEST_CASE("a foreach binding gets an inlay hint", "[lsp]")
{
    // a range is the standard library's, and so is the iteration protocol foreach lowers through
    Compiler::DriverOptions driver = lsp_driver();
    driver.no_stdlib = false;
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-inlay-foreach.eco";
    const std::string source
        = "function main() : void {\n"
          "    foreach (0 .. 3 as $i) {\n"
          "        echo $i;\n"
          "    }\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    bool saw = false;
    for (const auto &hint : Compiler::Lsp::inlay_hints(*session.snapshot(), *file, 1, 100)) {
        if (hint.position.line == 2 && hint.position.column == location_of(source, "$i").column) {
            saw = true;
        }
    }

    REQUIRE(saw);
}
