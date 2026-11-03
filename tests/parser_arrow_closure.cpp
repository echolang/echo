#include <catch2/catch_test_macros.hpp>

#include <Parser/FuncDeclParser.h>

#include "helpers.h"

// Parser::starts_arrow_closure owns "(params) =>". grouping (1 + 2) and ($x) without =>
// must fail the scan, and the cursor must not move

namespace
{
    bool starts_arrow(const std::string &content)
    {
        auto env = EchoTests::tests_make_parser_env(content);

        const auto before = env.payload.cursor.snapshot();
        const bool answer = Parser::starts_arrow_closure(env.payload.cursor);
        const auto after = env.payload.cursor.snapshot();

        REQUIRE(after.index == before.index);
        REQUIRE(after.shr_split_index == before.shr_split_index);
        REQUIRE(env.collector->issues.size() == 0);

        return answer;
    }
}

TEST_CASE("an arrow closure is recognised by (params) =>", "[arrow_closure]")
{
    REQUIRE(starts_arrow("() => 1"));
    REQUIRE(starts_arrow("(int32 $a) => $a + 1"));
    REQUIRE(starts_arrow("(int32 $a, int32 $b) => $a + $b"));
    REQUIRE(starts_arrow("(int32 $a) : int32 => $a"));
    REQUIRE(starts_arrow("($a) => $a"));
    REQUIRE(starts_arrow("(mv string $s) => $s"));
    REQUIRE(starts_arrow("(read int32& $a) => $a"));
    REQUIRE(starts_arrow("(function<int32(int32)> $f) => $f(1)"));
    REQUIRE(starts_arrow("(int32 $a = 1) => $a"));
    REQUIRE(starts_arrow("(int32 $a = ['k' => 1]) => $a"));
    REQUIRE(starts_arrow("(Box<Box<int32>> $b) => $b"));
    REQUIRE(starts_arrow("(from: int32 $a) => $a"));
    REQUIRE(starts_arrow("(int32 $a,) => $a"));
    REQUIRE(starts_arrow("(int32 $a = (int32 $x) : int32 => $x) => $a"));
}

TEST_CASE("grouping is not an arrow closure", "[arrow_closure]")
{
    REQUIRE_FALSE(starts_arrow("(1 + 2)"));
    REQUIRE_FALSE(starts_arrow("($x)"));
    REQUIRE_FALSE(starts_arrow("(foo())"));
    REQUIRE_FALSE(starts_arrow("(int32 $a)"));
    REQUIRE_FALSE(starts_arrow("['k' => 1]"));
    REQUIRE_FALSE(starts_arrow("function(int32 $a) : int32 { return $a; }"));
    REQUIRE_FALSE(starts_arrow("(1 + 2) => 3"));
    REQUIRE_FALSE(starts_arrow("match ($x) { .a => 1 }"));
}
