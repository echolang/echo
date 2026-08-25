#include <catch2/catch_test_macros.hpp>

#include <Compiler/DriverOptions.h>
#include <Compiler/Lsp/LspQuery.h>
#include <Compiler/Lsp/LspSession.h>
#include <Compiler/Lsp/LspUri.h>

#include <filesystem>
#include <string>

TEST_CASE("document symbols name functions and types in a loose file", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    session.set_workspace_root("/tmp");

    const std::filesystem::path path = "/tmp/symbols.eco";
    session.did_open(path, 1,
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    function mag() : int32 { return 0; }\n"
        "}\n"
        "function origin() : Point { return Point(0, 0); }\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);
    const auto symbols = Compiler::Lsp::document_symbols(*file);
    REQUIRE_FALSE(symbols.empty());

    bool saw_point = false;
    bool saw_origin = false;
    bool mag_at_file_root = false;
    for (const auto &symbol : symbols) {
        if (symbol.name == "Point") {
            saw_point = true;
            REQUIRE(symbol.kind == Compiler::Lsp::OutlineKind::t_struct);

            bool saw_x = false;
            bool saw_y = false;
            bool saw_mag = false;
            for (const auto &child : symbol.children) {
                if (child.name == "$x") {
                    saw_x = true;
                    REQUIRE(child.kind == Compiler::Lsp::OutlineKind::t_property);
                }
                if (child.name == "$y") {
                    saw_y = true;
                }
                if (child.name == "mag") {
                    saw_mag = true;
                    REQUIRE(child.kind == Compiler::Lsp::OutlineKind::t_method);
                }
            }

            REQUIRE(saw_x);
            REQUIRE(saw_y);
            REQUIRE(saw_mag);
        }
        if (symbol.name == "origin") {
            saw_origin = true;
            REQUIRE(symbol.kind == Compiler::Lsp::OutlineKind::t_function);
            REQUIRE(AST::location_before(symbol.selection.end, symbol.range.end));
        }
        if (symbol.name == "mag") {
            mag_at_file_root = true;
        }
    }

    REQUIRE(saw_point);
    REQUIRE(saw_origin);
    REQUIRE_FALSE(mag_at_file_root);
}

TEST_CASE("a test block is not a document symbol", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/tests-hidden.eco";
    session.did_open(path, 1,
        "function add(int32 $a) : int32 { return $a; }\n"
        "test adds_up { }\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);
    const auto symbols = Compiler::Lsp::document_symbols(*file);
    for (const auto &symbol : symbols) {
        REQUIRE(symbol.name != "adds_up");
        REQUIRE(symbol.name.find("test$") == std::string::npos);
    }
}

TEST_CASE("declarations after a namespace statement are children of it", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-namespace.eco";
    session.did_open(path, 1,
        "namespace geom;\n"
        "struct Point { int32 $x; }\n"
        "function origin() : Point { return Point(0); }\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);
    const auto symbols = Compiler::Lsp::document_symbols(*file);

    bool saw_geom = false;
    bool point_at_root = false;
    bool origin_at_root = false;
    for (const auto &symbol : symbols) {
        if (symbol.name == "geom") {
            saw_geom = true;
            REQUIRE(symbol.kind == Compiler::Lsp::OutlineKind::t_namespace);

            bool saw_point = false;
            bool saw_origin = false;
            for (const auto &child : symbol.children) {
                if (child.name == "Point") {
                    saw_point = true;
                }
                if (child.name == "origin") {
                    saw_origin = true;
                }
            }

            REQUIRE(saw_point);
            REQUIRE(saw_origin);
        }
        if (symbol.name == "Point") {
            point_at_root = true;
        }
        if (symbol.name == "origin") {
            origin_at_root = true;
        }
    }

    REQUIRE(saw_geom);
    REQUIRE_FALSE(point_at_root);
    REQUIRE_FALSE(origin_at_root);
}
