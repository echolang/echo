#include <catch2/catch_test_macros.hpp>

#include <AST/ASTStringLiteral.h>

TEST_CASE("a brace glued to an identifier looks like a hole", "[string][interpolation]")
{
    REQUIRE(AST::interpolation_lookalike("{twice($n)}") == "{twice($n)}");
    REQUIRE(AST::interpolation_lookalike("call: {twice($n)}") == "{twice($n)}");
    REQUIRE(AST::interpolation_lookalike("{LIMIT}") == "{LIMIT}");
    REQUIRE(AST::interpolation_lookalike("const: {LIMIT} after") == "{LIMIT}");
}

TEST_CASE("a hole lookalike is not a space after the brace, an escape, or a real hole", "[string][interpolation]")
{
    REQUIRE_FALSE(AST::interpolation_lookalike("a { brace").has_value());
    REQUIRE_FALSE(AST::interpolation_lookalike("a { brace and a } brace").has_value());
    REQUIRE_FALSE(AST::interpolation_lookalike("\\{twice($n)}").has_value());
    REQUIRE_FALSE(AST::interpolation_lookalike("{$n}").has_value());
    REQUIRE_FALSE(AST::interpolation_lookalike("").has_value());
    REQUIRE_FALSE(AST::interpolation_lookalike("{").has_value());
    REQUIRE_FALSE(AST::interpolation_lookalike("{ 1}").has_value());
}
