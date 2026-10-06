#include <catch2/catch_test_macros.hpp>

#include <AST/ASTFile.h>
#include <Compiler/DriverOptions.h>
#include <Compiler/Lsp/LspRename.h>
#include <Compiler/Lsp/LspSession.h>
#include <Compiler/SettledPath.h>

#include <algorithm>
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

    AST::Location location_of(const std::string &source, const std::string &needle, size_t nth = 0)
    {
        size_t at = source.find(needle);
        for (size_t i = 0; i < nth && at != std::string::npos; i++) {
            at = source.find(needle, at + 1);
        }
        REQUIRE(at != std::string::npos);

        const size_t newline = source.rfind('\n', at);
        const size_t line_begin = newline == std::string::npos ? 0 : newline + 1;
        const uint32_t line = static_cast<uint32_t>(std::count(source.begin(), source.begin() + at, '\n') + 1);
        return AST::Location{ line, static_cast<uint32_t>(at - line_begin + 1) };
    }

    // the source with every edit applied, so a test reads as before and after
    std::string applied(const std::string &source, const std::filesystem::path &path, std::vector<Compiler::Lsp::RenameEdit> edits)
    {
        // File::get_path is settled: `/tmp/...` is `D:\tmp\...` on Windows
        const std::filesystem::path settled = Compiler::canonical_or_absolute(path);
        std::vector<std::pair<size_t, Compiler::Lsp::RenameEdit>> located;
        for (const auto &edit : edits) {
            if (Compiler::canonical_or_absolute(edit.path) != settled) {
                continue;
            }

            size_t offset = 0;
            for (uint32_t line = 1; line < edit.range.start.line; line++) {
                offset = source.find('\n', offset) + 1;
            }
            located.push_back({ offset + edit.range.start.column - 1, edit });
        }

        std::sort(located.begin(), located.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::string out = source;
        for (const auto &[offset, edit] : located) {
            out.replace(offset, edit.range.end.column - edit.range.start.column, edit.new_text);
        }
        return out;
    }

    Compiler::Lsp::RenameAnswer rename_at(
        Compiler::Lsp::Session &session,
        const std::filesystem::path &path,
        const std::string &source,
        const std::string &needle,
        const std::string &new_name,
        size_t nth = 0
    )
    {
        const AST::File *file = session.file_of(path);
        REQUIRE(file != nullptr);
        return Compiler::Lsp::rename(*session.snapshot(), *file, location_of(source, needle, nth), new_name, {});
    }
};

TEST_CASE("rename: a local changes in its own function only", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-local.eco";
    const std::string source
        = "function a() : int32 { int32 $n = 1; $n = $n + 1; return $n; }\n"
          "function b() : int32 { int32 $n = 2; return $n; }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "$n", "count", 2);
    REQUIRE(answer.refusal.empty());
    REQUIRE(applied(source, path, answer.edits)
        == "function a() : int32 { int32 $count = 1; $count = $count + 1; return $count; }\n"
           "function b() : int32 { int32 $n = 2; return $n; }\n");
}

TEST_CASE("rename: a property changes at its declaration, its uses and its named arguments", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-property.eco";
    const std::string source
        = "struct Point { int32 $x; int32 $y; }\n"
          "function main() : void {\n"
          "    Point $p = Point($x: 1, $y: 2);\n"
          "    $p->x = 3;\n"
          "    echo $p->x;\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "x;", "left");
    REQUIRE(answer.refusal.empty());
    REQUIRE(applied(source, path, answer.edits)
        == "struct Point { int32 $left; int32 $y; }\n"
           "function main() : void {\n"
           "    Point $p = Point($left: 1, $y: 2);\n"
           "    $p->left = 3;\n"
           "    echo $p->left;\n"
           "}\n");
}

TEST_CASE("rename: a type changes in annotations and constructor calls", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-type.eco";
    const std::string source
        = "struct Point { int32 $x; }\n"
          "function make() : Point { return Point(1); }\n"
          "function main() : void { Point $p = make(); echo $p->x; }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "Point", "Vec2", 3);
    REQUIRE(answer.refusal.empty());
    REQUIRE(applied(source, path, answer.edits)
        == "struct Vec2 { int32 $x; }\n"
           "function make() : Vec2 { return Vec2(1); }\n"
           "function main() : void { Vec2 $p = make(); echo $p->x; }\n");
}

TEST_CASE("rename: a parameter changes at named arguments", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-param.eco";
    const std::string source
        = "function pair(int32 $a, int32 $b) : int32 { return $a * 10 + $b; }\n"
          "function main() : void { echo pair($b: 2, $a: 1); }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "$a", "first");
    REQUIRE(answer.refusal.empty());
    REQUIRE(applied(source, path, answer.edits)
        == "function pair(int32 $first, int32 $b) : int32 { return $first * 10 + $b; }\n"
           "function main() : void { echo pair($b: 2, $first: 1); }\n");
}

TEST_CASE("rename: a function changes across files and in its use; an alias stays put", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path lib = "/tmp/lsp-rename-geo.eco";
    const std::filesystem::path app = "/tmp/lsp-rename-app.eco";
    const std::filesystem::path alias = "/tmp/lsp-rename-alias.eco";
    const std::string lib_source = "namespace geo;\nfunction area(int32 $w) : int32 { return $w * $w; }\n";
    const std::string app_source = "use geo::area;\nfunction main() : void { echo area(2); echo geo::area(3); }\n";
    const std::string alias_source = "use geo::area as size;\nfunction other() : void { echo size(4); }\n";
    session.did_open(lib, 1, lib_source);
    session.did_open(app, 1, app_source);
    session.did_open(alias, 1, alias_source);
    session.rebuild();

    const auto answer = rename_at(session, lib, lib_source, "area", "surface");
    REQUIRE(answer.refusal.empty());
    REQUIRE(applied(lib_source, lib, answer.edits)
        == "namespace geo;\nfunction surface(int32 $w) : int32 { return $w * $w; }\n");
    REQUIRE(applied(app_source, app, answer.edits)
        == "use geo::surface;\nfunction main() : void { echo surface(2); echo geo::surface(3); }\n");
    REQUIRE(applied(alias_source, alias, answer.edits)
        == "use geo::surface as size;\nfunction other() : void { echo size(4); }\n");
}

TEST_CASE("rename: refuses a new name that is not an identifier", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-invalid.eco";
    const std::string source
        = "struct Point { int32 $x; function len() : int32 { return $this->x; } }\n"
          "function twice(int32 $a) : int32 { return $a * 2; }\n"
          "function half(int32 $a) : int32 { return $a / 2; }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    REQUIRE_FALSE(rename_at(session, path, source, "twice", "1x").refusal.empty());
    REQUIRE_FALSE(rename_at(session, path, source, "twice", "foreach").refusal.empty());
    REQUIRE_FALSE(rename_at(session, path, source, "twice", "$twice").refusal.empty());
    REQUIRE_FALSE(rename_at(session, path, source, "twice", "half").refusal.empty());
    REQUIRE_FALSE(rename_at(session, path, source, "Point", "int32").refusal.empty());
    REQUIRE_FALSE(rename_at(session, path, source, "$this", "self").refusal.empty());

    // the sigil is optional on a variable
    REQUIRE(rename_at(session, path, source, "$a", "$value").refusal.empty());
}

TEST_CASE("rename: refuses a name also written in a test or inactive #[if:]", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-test-block.eco";
    const std::string source
        = "function twice(int32 $a) : int32 { return $a * 2; }\n"
          "test doubles\n"
          "{\n"
          "    assert(twice(2) == 4);\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "twice", "triple");
    REQUIRE_FALSE(answer.refusal.empty());
    REQUIRE(answer.refusal.find("lsp-rename-test-block.eco:4") != std::string::npos);
    REQUIRE(answer.edits.empty());
}

TEST_CASE("rename: refuses a name inside an outer #[if:] after an inner #[end]", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-nested-if.eco";
    const std::string source
        = "function twice(int32 $a) : int32 { return $a * 2; }\n"
          "function main() : void { echo twice(1); }\n"
          "#[if: NEVER_DEFINED]\n"
          "    #[if: NEVER_DEFINED]\n"
          "        function inner() : void {}\n"
          "    #[end]\n"
          "    echo twice(2);\n"
          "#[end]\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "twice", "triple");
    REQUIRE_FALSE(answer.refusal.empty());
    REQUIRE(answer.refusal.find("lsp-rename-nested-if.eco:7") != std::string::npos);
    REQUIRE(answer.edits.empty());
}

TEST_CASE("rename: refuses a namespaced constant colliding with another in that namespace", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-const-ns.eco";
    const std::string source
        = "namespace geo;\n"
          "const MAX = 1;\n"
          "const MIN = 0;\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "MAX", "MIN");
    REQUIRE(answer.refusal == "'MIN' is already declared in that namespace.");
    REQUIRE(answer.edits.empty());
}

TEST_CASE("rename: refuses a local colliding with another in the same function", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-local-collision.eco";
    const std::string source
        = "function add() : int32 { int32 $a = 1; int32 $b = 2; return $a + $b; }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "$a", "b");
    REQUIRE(answer.refusal == "'$b' is already declared in this function.");
    REQUIRE(answer.edits.empty());
}

TEST_CASE("rename: refuses what the standard library declares", "[lsp][stdlib]")
{
    Compiler::DriverOptions driver = lsp_driver();
    driver.no_stdlib = false;
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-rename-stdlib.eco";
    const std::string source = "function main() : void { string $s = \"hi\"; echo $s->size(); }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = rename_at(session, path, source, "size", "length");
    REQUIRE(answer.refusal.find("standard library") != std::string::npos);

    const AST::File *file = session.file_of(path);
    REQUIRE_FALSE(Compiler::Lsp::prepare_rename(*session.snapshot(), *file, location_of(source, "size"), {}).refusal.empty());
    REQUIRE(Compiler::Lsp::prepare_rename(*session.snapshot(), *file, location_of(source, "$s"), {}).placeholder == "$s");
}
