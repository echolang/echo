#include <catch2/catch_test_macros.hpp>

#include <AST/ASTDiagnostic.h>
#include <AST/ASTFile.h>
#include <Compiler/DriverOptions.h>
#include <Compiler/Lsp/LspQuery.h>
#include <Compiler/Lsp/LspSession.h>

#include <filesystem>
#include <string>

namespace
{
    const Compiler::Lsp::Snapshot &require_snapshot(Compiler::Lsp::Session &session)
    {
        REQUIRE(session.snapshot() != nullptr);
        return *session.snapshot();
    }
};

TEST_CASE("didOpen of a broken file publishes a diagnostic", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/broken.eco";
    session.did_open(path, 1, "function main() : void { echo $missing; }\n");
    session.rebuild();

    bool saw_error = false;
    for (const auto &diagnostic : session.diagnostics()) {
        if (diagnostic.severity == AST::IssueSeverity::Error) {
            saw_error = true;
        }
    }

    REQUIRE(saw_error);
}

TEST_CASE("didChange that fixes the file publishes an empty list", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/fixable.eco";
    session.did_open(path, 1, "function main() : void { echo $missing; }\n");
    session.rebuild();
    REQUIRE_FALSE(session.diagnostics().empty());

    session.did_change(path, 2, "function main() : void { echo 1; }\n");
    session.rebuild();

    bool cleared = true;
    for (const auto &diagnostic : session.diagnostics()) {
        if (diagnostic.severity == AST::IssueSeverity::Error) {
            cleared = false;
        }
    }

    REQUIRE(cleared);
}

TEST_CASE("didClose drops the overlay so disk is truth", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/closed.eco";
    session.did_open(path, 1, "function main() : void { echo 1; }\n");
    session.rebuild();
    REQUIRE(session.dirty() == false);

    session.did_close(path);
    REQUIRE(session.dirty());
}

TEST_CASE("references of a function include the declaration and each call", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-refs.eco";
    const std::string source
        = "function add(int32 $a) : int32 { return $a; }\n"
          "function main() : void { echo add(1); echo add(2); }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t call = source.find("add(1)");
    REQUIRE(call != std::string::npos);
    const size_t line2 = source.find('\n') + 1;
    const AST::Location location{ 2, static_cast<uint32_t>(call - line2 + 1) };

    const auto hits = Compiler::Lsp::references(require_snapshot(session), *file, location, true);
    REQUIRE(hits.size() >= 3);
}

TEST_CASE("workspace symbols match a nested method by name", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-workspace.eco";
    session.did_open(path, 1,
        "struct Point {\n"
        "    function mag() : int32 { return 0; }\n"
        "}\n");
    session.rebuild();

    bool saw_mag = false;
    for (const auto &symbol : Compiler::Lsp::workspace_symbols(require_snapshot(session), "mag")) {
        if (symbol.name == "mag") {
            saw_mag = true;
            REQUIRE(symbol.container == "Point");
            REQUIRE(symbol.kind == Compiler::Lsp::OutlineKind::t_method);
        }
    }

    REQUIRE(saw_mag);
}

TEST_CASE("signature help names the active argument of a call", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-signature.eco";
    const std::string source
        = "function add(int32 $a, int32 $b) : int32 { return $a; }\n"
          "function main() : void { echo add(1, 2); }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t line2 = source.find('\n') + 1;
    const size_t comma = source.find(", 2)");
    REQUIRE(comma != std::string::npos);
    const AST::Location after_comma{ 2, static_cast<uint32_t>(comma - line2 + 2) };

    const auto help = Compiler::Lsp::signature_help(require_snapshot(session), *file, after_comma);
    REQUIRE(help.has_value());
    REQUIRE(help->parameters.size() == 2);
    REQUIRE(help->parameters[0].find("$a") != std::string::npos);
    REQUIRE(help->parameters[1].find("$b") != std::string::npos);
    REQUIRE(help->active_parameter == 1);
}

TEST_CASE("hover and definition work on a constant use", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-const-use.eco";
    const std::string line1 = "const uint32 EXTENSIONS = 0x1F03;";
    const std::string line2 = "function main() : void { echo EXTENSIONS; }";
    session.did_open(path, 1, line1 + "\n" + line2 + "\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t col = line2.find("EXTENSIONS");
    REQUIRE(col != std::string::npos);
    const AST::Location at_use{ 2, static_cast<uint32_t>(col + 1) };

    const auto hover = Compiler::Lsp::hover(require_snapshot(session), *file, at_use);
    REQUIRE(hover.has_value());
    REQUIRE(hover->type_description.find("uint32") != std::string::npos);

    const auto def = Compiler::Lsp::definition(require_snapshot(session), *file, at_use);
    REQUIRE(def.has_value());
    REQUIRE(def->range.start.line == 1);
}

TEST_CASE("hover and definition work on a const parameter type", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-param-type.eco";
    const std::string line1 = "struct Mat4 { int32 $e; }";
    const std::string line2 = "function matFloats(const Mat4& $m) : void { echo $m->$e; }";
    session.did_open(path, 1, line1 + "\n" + line2 + "\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t col = line2.find("Mat4");
    REQUIRE(col != std::string::npos);
    const AST::Location at_type{ 2, static_cast<uint32_t>(col + 1) };

    const auto hover = Compiler::Lsp::hover(require_snapshot(session), *file, at_type);
    REQUIRE(hover.has_value());
    REQUIRE(hover->type_description.find("Mat4") != std::string::npos);

    const auto def = Compiler::Lsp::definition(require_snapshot(session), *file, at_type);
    REQUIRE(def.has_value());
    REQUIRE(def->range.start.line == 1);
}

TEST_CASE("go-to-definition on a type parameter argument is a miss, not the outer type", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-nested-tparam.eco";
    const std::string line1 = "struct Box<T> { T $v; }";
    const std::string line2 = "function wrap<T>(Box<T> $b) : T { return $b->$v; }";
    session.did_open(path, 1, line1 + "\n" + line2 + "\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t col = line2.find("Box<T>") + 4;
    REQUIRE(col != std::string::npos);
    const AST::Location at_t{ 2, static_cast<uint32_t>(col + 1) };

    const auto hover = Compiler::Lsp::hover(require_snapshot(session), *file, at_t);
    REQUIRE(hover.has_value());
    REQUIRE(hover->type_description.find("Box") == std::string::npos);

    const auto def = Compiler::Lsp::definition(require_snapshot(session), *file, at_t);
    REQUIRE_FALSE(def.has_value());
}

TEST_CASE("go-to-definition on a primitive type argument is a miss, not the outer type", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-nested-primitive.eco";
    const std::string line1 = "struct Box<T> { T $v; }";
    const std::string line2 = "function take(Box<int32> $b) : void { echo $b->$v; }";
    session.did_open(path, 1, line1 + "\n" + line2 + "\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t col = line2.find("int32");
    REQUIRE(col != std::string::npos);
    const AST::Location at_int{ 2, static_cast<uint32_t>(col + 1) };

    const auto hover = Compiler::Lsp::hover(require_snapshot(session), *file, at_int);
    REQUIRE(hover.has_value());
    REQUIRE(hover->type_description.find("int32") != std::string::npos);
    REQUIRE(hover->type_description.find("Box") == std::string::npos);

    const auto def = Compiler::Lsp::definition(require_snapshot(session), *file, at_int);
    REQUIRE_FALSE(def.has_value());
}

TEST_CASE("go-to-definition on a nested type argument names that type, not the outer one", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-nested-type.eco";
    const std::string line1 = "struct Foo { int32 $x; }";
    const std::string line2 = "struct Box<T> { T $v; }";
    const std::string line3 = "function take(Box<Foo> $b) : void { echo $b->$v->$x; }";
    session.did_open(path, 1, line1 + "\n" + line2 + "\n" + line3 + "\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t col = line3.find("Foo");
    REQUIRE(col != std::string::npos);
    const AST::Location at_foo{ 3, static_cast<uint32_t>(col + 1) };

    const auto hover = Compiler::Lsp::hover(require_snapshot(session), *file, at_foo);
    REQUIRE(hover.has_value());
    REQUIRE(hover->type_description.find("Foo") != std::string::npos);
    REQUIRE(hover->type_description.find("Box") == std::string::npos);

    const auto def = Compiler::Lsp::definition(require_snapshot(session), *file, at_foo);
    REQUIRE(def.has_value());
    REQUIRE(def->range.start.line == 1);
}

TEST_CASE("rebuild reports how long parse and checking took", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    session.did_open("/tmp/lsp-timing.eco", 1, "function main() : void { echo 1; }\n");
    const auto report = session.rebuild();
    REQUIRE(report.failed == false);
    REQUIRE(report.total_ms >= 0);
    REQUIRE(report.files >= 1);
}

TEST_CASE("rebuild waits for an in-flight idle compile and does not keep a stale snapshot", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path first = "/tmp/lsp-gen-a.eco";
    const std::filesystem::path second = "/tmp/lsp-gen-b.eco";
    session.did_open(first, 1, "function main() : void { echo 1; }\n");
    session.start_rebuild("idle");
    session.did_open(second, 1, "function other() : void { echo 2; }\n");

    const auto report = session.rebuild();
    REQUIRE(report.failed == false);
    REQUIRE(session.file_of(first) != nullptr);
    REQUIRE(session.file_of(second) != nullptr);
}

TEST_CASE("didChange with the same bytes does not mark the session dirty", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-unchanged.eco";
    const std::string source = "function main() : void { echo 1; }\n";
    session.did_open(path, 1, source);
    session.rebuild();
    REQUIRE(session.dirty() == false);

    session.did_change(path, 2, source);
    REQUIRE(session.dirty() == false);
}

TEST_CASE("go-to-definition on a function name goes to the declaration, not a call", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-def-not-call.eco";
    const std::string line1 = "function add(int32 $a) : int32 { return $a; }";
    const std::string line2 = "function main() : void { echo add(1); echo add(2); }";
    session.did_open(path, 1, line1 + "\n" + line2 + "\n");
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t decl_col = line1.find("add");
    REQUIRE(decl_col != std::string::npos);
    const auto from_decl = Compiler::Lsp::definition(
        require_snapshot(session), *file, AST::Location{ 1, static_cast<uint32_t>(decl_col + 1) });
    REQUIRE(from_decl.has_value());
    REQUIRE(from_decl->range.start.line == 1);

    const size_t call_col = line2.find("add(1)");
    REQUIRE(call_col != std::string::npos);
    const auto from_call = Compiler::Lsp::definition(
        require_snapshot(session), *file, AST::Location{ 2, static_cast<uint32_t>(call_col + 1) });
    REQUIRE(from_call.has_value());
    REQUIRE(from_call->range.start.line == 1);
}

TEST_CASE("go-to-definition works for a file that exists only in the overlay", "[lsp]")
{
    Compiler::DriverOptions driver;
    driver.subcommand = Compiler::Subcommand::t_lsp;
    driver.no_stdlib = true;

    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-overlay-only-never-saved.eco";
    const std::string source
        = "function add(int32 $a) : int32 { return $a; }\n"
          "function main() : void { echo add(1); }\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const AST::File *file = session.file_of(path);
    REQUIRE(file != nullptr);

    const size_t line2 = source.find('\n') + 1;
    const size_t call = source.find("add(1)");
    REQUIRE(call != std::string::npos);
    const AST::Location location{ 2, static_cast<uint32_t>(call - line2 + 1) };

    const auto hit = Compiler::Lsp::definition(require_snapshot(session), *file, location);
    REQUIRE(hit.has_value());
    REQUIRE(hit->path == path);
}
