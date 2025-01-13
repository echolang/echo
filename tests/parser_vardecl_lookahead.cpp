#include <catch2/catch_test_macros.hpp>

#include <Parser/TypeParser.h>

#include "helpers.h"

// `Parser::starts_vardecl` is the one owner of "what a declaration looks like" - the statement
// dispatch and the struct member loop both ask it, which is why a generic property and a generic
// local were one defect rather than two
//
// it answers by scanning the *type grammar* and looking at what follows, rather than by enumerating
// token sequences. these cases pin both halves of that: every spelling a type has in front of a
// variable name, and the things that look like one and are not

namespace
{
    // the predicate's answer for `content`, with the cursor at the first token. also asserts the
    // scan left the cursor where it found it - the two callers both go on to read from it, and a
    // lookahead that moved would send them into the middle of a type
    bool starts_vardecl(const std::string &content)
    {
        auto env = EchoTests::tests_make_parser_env(content);

        const auto before = env.payload.cursor.snapshot();
        const bool answer = Parser::starts_vardecl(env.payload);
        const auto after = env.payload.cursor.snapshot();

        REQUIRE(after.index == before.index);
        REQUIRE(after.shr_split_index == before.shr_split_index);

        // pure lookahead in the other sense too: nothing is reported, and nothing is resolved, so it
        // can run before the type-name pass has reached the type it is looking at
        REQUIRE(env.collector->issues.size() == 0);

        return answer;
    }
}

TEST_CASE("a declaration is recognised in every spelling of its type", "[vardecl_lookahead]")
{
    // the spellings that already worked
    REQUIRE(starts_vardecl("int32 $x = 5;"));
    REQUIRE(starts_vardecl("int32 $x;"));
    REQUIRE(starts_vardecl("$x = 5;"));
    REQUIRE(starts_vardecl("const int32 $x = 5;"));
    REQUIRE(starts_vardecl("ptr<int32> $p = null;"));
    REQUIRE(starts_vardecl("int32& $r = &$x;"));
    REQUIRE(starts_vardecl("int32 & $r = &$x;")); // the spaced `&` lexes as t_and, not t_ref
    REQUIRE(starts_vardecl("a::b::Foo $f;"));

    // a generic application, which had no arm at all - the whole of B22
    REQUIRE(starts_vardecl("Q<int32> $q = Q<int32>(2);"));
    REQUIRE(starts_vardecl("Q<int32> $q;"));

    // nested, closing on a single `>>` token the scan has to split
    REQUIRE(starts_vardecl("Box<Box<int32>> $b;"));
    REQUIRE(starts_vardecl("Box<Box<Box<int32>>> $b;"));

    // more than one type argument
    REQUIRE(starts_vardecl("Pair<int32, float64> $p;"));
    REQUIRE(starts_vardecl("Pair<Box<int32>, float64> $p;"));

    // combinations the three old helpers could not reach, because each knew one feature and they
    // did not compose: a borrow of a generic, a qualified borrow, a qualified generic
    REQUIRE(starts_vardecl("Q<int32>& $r;"));
    REQUIRE(starts_vardecl("a::b::Foo& $f;"));
    REQUIRE(starts_vardecl("a::b::Q<int32> $q;"));
    REQUIRE(starts_vardecl("const Q<int32>& $q;"));
    REQUIRE(starts_vardecl("ptr<Q<int32>> $p;"));
    REQUIRE(starts_vardecl("ptr<Box<Box<int32>>> $p;"));
}

TEST_CASE("something that only looks like a declaration is not one", "[vardecl_lookahead]")
{
    // a constructor call used as a statement: a whole type, but a `(` after it rather than a name.
    // this is the case the dispatch's next branch handles, and the reason the scan alone is not the
    // answer - what follows the type is
    REQUIRE_FALSE(starts_vardecl("Q<int32>(2);"));
    REQUIRE_FALSE(starts_vardecl("Foo(2);"));
    REQUIRE_FALSE(starts_vardecl("a::b::foo();"));

    // an explicitly parameterised call
    REQUIRE_FALSE(starts_vardecl("box<int32>(5);"));

    // a comparison. safe because a bare identifier is never a value operand - values carry a `$` -
    // so nothing here even enters the scan
    REQUIRE_FALSE(starts_vardecl("$a < $b;"));
    REQUIRE_FALSE(starts_vardecl("$a > $b;"));

    // a member write and a re-seat are statements, not declarations. the dispatch lists them beside
    // this predicate rather than inside it
    REQUIRE_FALSE(starts_vardecl("$s->x = 1;"));
    REQUIRE_FALSE(starts_vardecl("$p:$ = &$q;"));

    // a truncated type runs the scan out of tokens rather than off the end of the collection
    REQUIRE_FALSE(starts_vardecl("Q<int32"));
    REQUIRE_FALSE(starts_vardecl("Q<"));
    REQUIRE_FALSE(starts_vardecl("a::"));
}

TEST_CASE("const and ptr answer without a scan", "[vardecl_lookahead]")
{
    // neither can begin anything else at a statement head, so a malformed one still reaches
    // parse_varexpr - which knows what it was reading and reports accordingly - instead of falling
    // through to the dispatch's catch-all
    REQUIRE(starts_vardecl("const $inferred = 10;"));
    REQUIRE(starts_vardecl("ptr<int32> $p"));
    REQUIRE(starts_vardecl("ptr"));
}
