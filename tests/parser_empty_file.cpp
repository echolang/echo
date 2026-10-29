#include <catch2/catch_test_macros.hpp>

#include <AST/ASTFile.h>
#include <AST/ASTFileRoot.h>
#include <AST/ScopeNode.h>
#include <Parser/ParserCursor.h>
#include <Token.h>

#include "helpers.h"
#include "subprocess.h"

// an empty file at the front of a module used to walk every later file's tokens: Cursor(tokens, 0, 0)
// treated `_end == 0` as unbounded. a comment-only file is the same (the lexer yields no tokens)

TEST_CASE("Cursor(tokens, 0, 0) is empty after later tokens exist", "[parser]")
{
    TokenCollection tokens;
    tokens.push("echo", Token::Type::t_identifier, 1, 1);
    tokens.push("1", Token::Type::t_integer_literal, 1, 6);

    Parser::Cursor empty(tokens, 0, 0);

    REQUIRE(empty.is_empty());
    REQUIRE(empty.is_done());
    REQUIRE(empty.range_size() == 0);

    Parser::Cursor rest(tokens, 0);

    REQUIRE_FALSE(rest.is_empty());
    REQUIRE(rest.range_size() == tokens.size());
}

TEST_CASE("an empty leading file does not parse later files as its own", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::vector<std::string>{"", "echo 1;"});

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::File *first = nullptr;
    AST::File *second = nullptr;

    for (AST::File &file : bundle->modules.find_module("test").files()) {
        if (first == nullptr) {
            first = &file;
        }
        else {
            second = &file;
        }
    }

    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->root != nullptr);
    REQUIRE(second->root != nullptr);
    REQUIRE(AST::first_top_level_statement(*first->root) == nullptr);
    REQUIRE(AST::first_top_level_statement(*second->root) != nullptr);
}

TEST_CASE("a comment-only leading file is the same empty slice", "[parser]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        std::vector<std::string>{"// nothing here\n", "echo 1;"});

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::File *first = nullptr;

    for (AST::File &file : bundle->modules.find_module("test").files()) {
        if (first == nullptr) {
            first = &file;
            break;
        }
    }

    REQUIRE(first != nullptr);
    REQUIRE(first->root != nullptr);
    REQUIRE(AST::first_top_level_statement(*first->root) == nullptr);
}

TEST_CASE("echoc run of an empty leading source prints the program once", "[parser]")
{
    EchoTests::ScopedProject project("empty_file", "leading");
    EchoTests::write_file(project.root() / "empty.eco", "");
    EchoTests::write_file(project.root() / "main.eco", "echo 'main';\n");

    const EchoTests::ProcessResult result = project.echoc("run --silent empty.eco main.eco");

    REQUIRE(result.exit_code == 0);
    REQUIRE(EchoTests::output_equals_lines(result.output, {"main"}));
}
