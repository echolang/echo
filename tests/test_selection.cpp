#include <catch2/catch_test_macros.hpp>

#include <Compiler/TestSelection.h>

// the two halves of choosing which tests run: what a `--filter` word *spells*, and what a selection
// *selects*. Both are pure functions over values, so they are asserted here rather than through a
// subprocess - the corpus already pins what a filter does to a real run.
//
// the grammar is deliberately the one `--link` reads, and that is what the refusals below are about: a
// misspelled tag has to be refused at the word that is wrong, because the alternative is reading it as a test
// name nothing carries and running nothing while reporting success

using Compiler::TestCase;
using Compiler::TestFilter;
using Compiler::TestFilterKind;
using Compiler::TestSelection;

namespace
{
    TestCase make_test(
        const std::string &module,
        const std::string &file,
        const std::string &group,
        const std::string &name)
    {
        return TestCase { module, file, group, name, "sym_" + name };
    }

    // the tests every selection case below chooses among. `mymath.eco` is there for one assertion only: it
    // is what tells a path *suffix* from a path substring
    std::vector<TestCase> corpus()
    {
        return {
            make_test("app", "src/math.eco", "fast", "adds_up"),
            make_test("app", "src/math.eco", "slow", "overflows"),
            make_test("app", "src/io.eco", "", "reads_a_line"),
            make_test("app", "src/mymath.eco", "", "not_math"),
            make_test("lib", "lib/src/math.eco", "fast", "adds_up"),
        };
    }

    std::vector<std::string> names_of(const std::vector<TestCase> &tests)
    {
        std::vector<std::string> names;

        for (const TestCase &test : tests) {
            names.push_back(Compiler::test_display_name(test));
        }

        return names;
    }

    TestSelection selection_of(const std::vector<std::string> &spelled)
    {
        TestSelection selection;

        for (const std::string &word : spelled) {
            TestFilter filter;
            std::string error;

            REQUIRE(Compiler::parse_test_filter(word, filter, error));
            selection.filters.push_back(filter);
        }

        return selection;
    }
};

TEST_CASE("a bare word is a test's name", "[test_selection]")
{
    TestFilter filter;
    std::string error;

    REQUIRE(Compiler::parse_test_filter("adds_up", filter, error));
    REQUIRE(filter.kind == TestFilterKind::t_name);
    REQUIRE(filter.value == "adds_up");
}

TEST_CASE("every tag in the vocabulary is readable, and `name` is spellable too", "[test_selection]")
{
    const std::vector<std::pair<std::string, TestFilterKind>> expected = {
        { "name", TestFilterKind::t_name },
        { "group", TestFilterKind::t_group },
        { "file", TestFilterKind::t_file },
        { "module", TestFilterKind::t_module },
    };

    for (const auto &[tag, kind] : expected) {
        TestFilter filter;
        std::string error;

        INFO(tag);
        REQUIRE(Compiler::parse_test_filter(tag + ":x", filter, error));
        REQUIRE(filter.kind == kind);
        REQUIRE(filter.value == "x");
    }
}

// **a misspelled tag is refused rather than read as a name**, which is the whole reason the bare case is
// decided by the absence of a colon rather than by asking the table first and falling back
TEST_CASE("a misspelled tag is refused at the word that is wrong", "[test_selection]")
{
    TestFilter filter;
    std::string error;

    REQUIRE_FALSE(Compiler::parse_test_filter("grup:math", filter, error));
    REQUIRE(error.find("grup") != std::string::npos);

    // the remedy is the vocabulary, so every tag is named
    for (const char *tag : { "name", "group", "file", "module" }) {
        INFO(tag);
        REQUIRE(error.find(tag) != std::string::npos);
    }
}

TEST_CASE("a tag with nothing after it is refused", "[test_selection]")
{
    TestFilter filter;
    std::string error;

    REQUIRE_FALSE(Compiler::parse_test_filter("group:", filter, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("no filters at all is every test", "[test_selection]")
{
    const TestSelection selection;

    REQUIRE(selection.is_everything());
    REQUIRE(Compiler::select_tests(corpus(), selection).size() == 5);
}

// **the filters add up.** Two `group:` words run both groups, which is only true if a test matching *any*
// filter is selected - intersecting them would make the second word narrow the first to nothing
TEST_CASE("two filters are a union and not an intersection", "[test_selection]")
{
    const std::vector<TestCase> selected
        = Compiler::select_tests(corpus(), selection_of({ "group:fast", "group:slow" }));

    REQUIRE(selected.size() == 3);
}

TEST_CASE("a name selects it in every file that declares one", "[test_selection]")
{
    const std::vector<TestCase> selected
        = Compiler::select_tests(corpus(), selection_of({ "adds_up" }));

    // **two of them, and that is the design**: a test's name is unique per file and not per module, so a name
    // is not an identity - the runner tells them apart by the file it reports beside each
    REQUIRE(names_of(selected) == std::vector<std::string> {
        "app/math.eco::adds_up", "lib/math.eco::adds_up" });
}

TEST_CASE("a module filter selects that module's tests", "[test_selection]")
{
    const std::vector<TestCase> selected
        = Compiler::select_tests(corpus(), selection_of({ "module:lib" }));

    REQUIRE(names_of(selected) == std::vector<std::string> { "lib/math.eco::adds_up" });
}

// **a suffix at a path boundary, so as little of a path as is unambiguous is enough to write.** A plain
// substring would have `math.eco` match `mymath.eco`, and a plain equality would need the whole path
TEST_CASE("a file filter matches a path suffix and stops at a separator", "[test_selection]")
{
    // the three in a file called `math.eco`, and never `mymath.eco` beside them
    REQUIRE(Compiler::select_tests(corpus(), selection_of({ "file:math.eco" })).size() == 3);
    REQUIRE(Compiler::select_tests(corpus(), selection_of({ "file:src/math.eco" })).size() == 3);
    REQUIRE(names_of(Compiler::select_tests(corpus(), selection_of({ "file:lib/src/math.eco" })))
        == std::vector<std::string> { "lib/math.eco::adds_up" });

    // a suffix that starts mid-word matches nothing at all
    REQUIRE(Compiler::select_tests(corpus(), selection_of({ "file:ath.eco" })).empty());
    REQUIRE(names_of(Compiler::select_tests(corpus(), selection_of({ "file:mymath.eco" })))
        == std::vector<std::string> { "app/mymath.eco::not_math" });
}

// an ungrouped test is not in the group `""`. There is no word for "the ungrouped ones" and inventing one out
// of an empty value silently would be worse than the refusal `group:` already earns
TEST_CASE("an ungrouped test matches no group", "[test_selection]")
{
    TestSelection selection;
    selection.filters.push_back(TestFilter { TestFilterKind::t_group, "" });

    REQUIRE(Compiler::select_tests(corpus(), selection).empty());
}

TEST_CASE("a display name is the module, the file and the name", "[test_selection]")
{
    REQUIRE(Compiler::test_display_name(make_test("app", "src/deep/math.eco", "fast", "adds_up"))
        == "app/math.eco::adds_up");
}
