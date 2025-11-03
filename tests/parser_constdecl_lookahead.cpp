#include <catch2/catch_test_macros.hpp>

#include <Parser/TypeParser.h>

#include "helpers.h"

// `Parser::starts_constdecl` and `Parser::starts_vardecl` are one question split on one token - the `$` on the
// name. Both spellings begin with `const`, so every dispatch site asks the constant one *first*, and the two
// have to disagree about every input: a spelling both claimed would be read as whichever arm came first, and a
// spelling neither claimed would fall through to the statement dispatch's catch-all.
//
// so these cases assert the *partition* rather than each predicate alone.

namespace
{
    struct Answers
    {
        bool constant;
        bool variable;
    };

    // both predicates' answers for `content`, with the cursor at the first token. each also has to leave the
    // cursor where it found it - the dispatch sites go on to read from it, and a lookahead that moved would
    // send the real parse into the middle of a type
    Answers answers_for(const std::string &content)
    {
        auto env = EchoTests::tests_make_parser_env(content);

        const auto before = env.payload.cursor.snapshot();

        const bool constant = Parser::starts_constdecl(env.payload);

        const auto after_constant = env.payload.cursor.snapshot();
        REQUIRE(after_constant.index == before.index);
        REQUIRE(after_constant.shr_split_index == before.shr_split_index);

        const bool variable = Parser::starts_vardecl(env.payload);

        const auto after_variable = env.payload.cursor.snapshot();
        REQUIRE(after_variable.index == before.index);
        REQUIRE(after_variable.shr_split_index == before.shr_split_index);

        // pure lookahead in the other sense too: nothing is reported, so either can run before the type-name
        // pass has reached the type it is looking at
        REQUIRE(env.collector->issues.size() == 0);

        return Answers { constant, variable };
    }

    bool is_constant(const std::string &content)
    {
        const Answers answers = answers_for(content);

        // the partition: a constant declaration is not also a variable declaration
        REQUIRE_FALSE((answers.variable && answers.constant));

        return answers.constant;
    }
}

TEST_CASE("a constant declaration is recognised by its $-less name", "[constdecl_lookahead]")
{
    // untyped, which is the arm that only the `=` distinguishes from a type standing alone
    REQUIRE(is_constant("const MAX = 100;"));

    // typed, in every spelling the type grammar has - the scan is skip_type_shape's, so all of these come
    // for free and are pinned to say so
    REQUIRE(is_constant("const usize MAX = 100;"));
    REQUIRE(is_constant("const int32 MAX = 100;"));
    REQUIRE(is_constant("const a::b::Foo THING = f();"));
    REQUIRE(is_constant("const Q<int32> THING = f();"));

    // nested generics, closing on a single `>>` token the scan has to split and then put back
    REQUIRE(is_constant("const Box<Box<int32>> THING = f();"));

    // and the shape with nothing assigned, which is a constant missing its value rather than something else.
    // claimed deliberately, so parse_constdecl is the one that gets to say so
    REQUIRE(is_constant("const MISSING;"));
}

TEST_CASE("a variable declaration is never read as a constant", "[constdecl_lookahead]")
{
    // the `$` is the whole difference, at every spelling
    REQUIRE_FALSE(is_constant("const $x = 5;"));
    REQUIRE_FALSE(is_constant("const usize $max = 100;"));
    REQUIRE_FALSE(is_constant("const int32 $x = 5;"));
    REQUIRE_FALSE(is_constant("const ptr<int32> $p = null;"));
    REQUIRE_FALSE(is_constant("const int32& $r = &$x;"));
    REQUIRE_FALSE(is_constant("const Box<Box<int32>> $b;"));

    // and these are still variable declarations, which is the half a wrong answer here would break
    REQUIRE(answers_for("const $x = 5;").variable);
    REQUIRE(answers_for("const usize $max = 100;").variable);
}

TEST_CASE("neither predicate claims something that is not a declaration", "[constdecl_lookahead]")
{
    // nothing without a leading `const` can be a constant declaration
    REQUIRE_FALSE(is_constant("MAX = 100;"));
    REQUIRE_FALSE(is_constant("echo MAX;"));
    REQUIRE_FALSE(is_constant("f(1);"));
    REQUIRE_FALSE(is_constant("Foo(1);"));

    // **`const function` is a method modifier**, not a constant named `function`. It answers no here and is
    // claimed by starts_funcdecl at every dispatch site, which is asked ahead of both of these
    REQUIRE_FALSE(is_constant("const function get() : int32 { return 1; }"));
}
