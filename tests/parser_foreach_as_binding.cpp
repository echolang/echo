#include <catch2/catch_test_macros.hpp>

#include <Parser/ForeachParser.h>

#include "helpers.h"

// `Parser::starts_foreach_as_binding` is the split that keeps foreach's `as` in the stream
// now that `$x as T` is a cast. parse_postfix_chain is the reader

namespace
{
    bool starts_foreach_as_binding(const std::string &content)
    {
        auto env = EchoTests::tests_make_parser_env(content);

        const auto before = env.payload.cursor.snapshot();
        const bool answer = Parser::starts_foreach_as_binding(env.payload.cursor);
        const auto after = env.payload.cursor.snapshot();

        REQUIRE(after.index == before.index);
        REQUIRE(after.shr_split_index == before.shr_split_index);
        REQUIRE(env.collector->issues.size() == 0);

        return answer;
    }
};

TEST_CASE("foreach's as is a binding, a type after as is a cast", "[foreach_as_binding][cast]")
{
    REQUIRE(starts_foreach_as_binding("as $el"));
    REQUIRE(starts_foreach_as_binding("as &$el"));
    REQUIRE(starts_foreach_as_binding("as & $el"));
    REQUIRE(starts_foreach_as_binding("as const &$el"));
    REQUIRE(starts_foreach_as_binding("as const & $el"));
    REQUIRE(starts_foreach_as_binding("as const $el"));

    REQUIRE_FALSE(starts_foreach_as_binding("as int32"));
    REQUIRE_FALSE(starts_foreach_as_binding("as const int32"));
    REQUIRE_FALSE(starts_foreach_as_binding("as Box<T>"));
    REQUIRE_FALSE(starts_foreach_as_binding("$el"));
}
