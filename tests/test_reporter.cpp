#include <catch2/catch_test_macros.hpp>

#include <Compiler/ProgressReporter.h>
#include <Compiler/TerminalCapabilities.h>
#include <Compiler/TestReporter.h>

#include <sstream>
#include <string>

#include "terminal_fixture.h"

// **what the e2e corpus cannot assert, for two independent reasons.**
//
// a `.test` golden is byte-exact and a duration is not reproducible, so a rendering that reports how long a
// test took can only be pinned where the milliseconds are a literal. And `--verbose`'s whole subject is what
// a *terminal* gets, which is the half of the two channels a pipe never sees - the same reason
// tests/progress.cpp exists beside the corpus rather than inside it.
//
// so every case below hands the reporter an ostringstream, a forced TerminalCapabilities and a TestResult it
// filled in itself. That is the seam Compiler::TestResult::milliseconds already provides: the runner
// measures, the reporter only ever renders

using Compiler::ProgressReporter;
using Compiler::TerminalCapabilities;
using EchoTests::a_terminal;
using Compiler::TestCase;
using Compiler::TestDetail;
using Compiler::TestOutcome;
using Compiler::TestReporter;
using Compiler::TestResult;

namespace
{
    TestResult a_result(
        const char *file,
        const char *name,
        unsigned int milliseconds = 0,
        TestOutcome outcome = TestOutcome::t_passed)
    {
        TestResult result;

        result.test = TestCase { "mylib", file, "", name, std::string("test$") + name };
        result.outcome = outcome;
        result.status = outcome == TestOutcome::t_passed ? 0 : 1;
        result.milliseconds = milliseconds;

        return result;
    }

    // the two glyphs the listing marks a line with, off the same theme the checklist draws
    constexpr const char *PASS = "✓";
    constexpr const char *FAIL = "✗";
};

TEST_CASE("the default rendering off a terminal is one line per test", "[testreporter]")
{
    // **the case the corpus rests on.** tests_eco/tests/*.test byte-compares exactly these lines, so this is
    // the assertion that says a new detail level added nothing to the old one
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal());

    reporter.begin(2);
    reporter.result(a_result("src/math.eco", "adds_up", 3));
    reporter.result(a_result("src/math.eco", "subtracts", 1, TestOutcome::t_failed));

    REQUIRE(!reporter.finish());

    REQUIRE(out.str() ==
        "ok   1/2  mylib/math.eco::adds_up\n"
        "FAIL 2/2  mylib/math.eco::subtracts  (exited 1)\n"
        "2 tests, 1 failed\n"
        "\n✗ mylib/math.eco::subtracts  exited 1\n");
}

TEST_CASE("an expected-death test that returned has its own sentence", "[testreporter]")
{
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal());

    TestResult result = a_result("src/math.eco", "should_die", 1, TestOutcome::t_failed);
    result.status = 0;
    result.test.expects_death = true;

    reporter.begin(1);
    reporter.result(result);

    REQUIRE(!reporter.finish());

    REQUIRE(out.str() ==
        "FAIL 1/1  mylib/math.eco::should_die  (expected death, but it returned)\n"
        "1 test, 1 failed\n"
        "\n✗ mylib/math.eco::should_die  expected death, but it returned\n");
}

TEST_CASE("a listing groups its tests under one header per file", "[testreporter]")
{
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal(), TestDetail::t_listing);

    reporter.begin(3);
    reporter.result(a_result("src/math.eco", "adds_up", 3));
    reporter.result(a_result("src/math.eco", "subtracts", 1));
    reporter.result(a_result("src/io.eco", "reads_a_line", 12));

    REQUIRE(reporter.finish());

    REQUIRE(out.str() ==
        "mylib/math.eco\n"
        "  ✓ adds_up                                             3 ms\n"
        "  ✓ subtracts                                           1 ms\n"
        "  2 tests, 4 ms\n"
        "\n"
        "mylib/io.eco\n"
        "  ✓ reads_a_line                                       12 ms\n"
        "  1 test, 12 ms\n"
        "\n"
        "3 tests passed\n");
}

TEST_CASE("a file's subtotal counts its own tests and nothing else", "[testreporter]")
{
    // the grouping is a comparison against the last file seen, so a file that comes back gets a second
    // header and a second subtotal rather than one wrong pair
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal(), TestDetail::t_listing);

    reporter.begin(3);
    reporter.result(a_result("src/math.eco", "adds_up", 2));
    reporter.result(a_result("src/io.eco", "reads_a_line", 5));
    reporter.result(a_result("src/math.eco", "subtracts", 1));

    REQUIRE(reporter.finish());

    REQUIRE(out.str().find("mylib/math.eco\n  ✓ adds_up") != std::string::npos);
    REQUIRE(out.str().find("  1 test, 2 ms") != std::string::npos);
    REQUIRE(out.str().find("  1 test, 5 ms") != std::string::npos);
    REQUIRE(out.str().find("  1 test, 1 ms") != std::string::npos);
    REQUIRE(out.str().find("3 tests passed") != std::string::npos);
}

TEST_CASE("a listing keeps a passing test's output and the default rendering keeps none", "[testreporter]")
{
    auto run = [](TestDetail detail) {
        std::ostringstream out;
        ProgressReporter progress;
        TestReporter reporter(out, progress, a_terminal(), detail);

        TestResult passed = a_result("src/math.eco", "adds_up", 1);
        passed.output = "counted to 42\n";

        reporter.begin(1);
        reporter.result(passed);
        reporter.finish();

        return out.str();
    };

    // the one thing this rendering buys that the other cannot: a `dprint` in a green test has no other way
    // out, the runner capturing every test's streams either way
    REQUIRE(run(TestDetail::t_listing).find("      counted to 42\n") != std::string::npos);
    REQUIRE(run(TestDetail::t_counter).find("counted to 42") == std::string::npos);
}

TEST_CASE("a failure is on its own line and still under the summary", "[testreporter]")
{
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal(), TestDetail::t_listing);

    TestResult failed = a_result("src/math.eco", "subtracts", 4, TestOutcome::t_failed);
    failed.output = "assertion failed: nope\n";

    reporter.begin(1);
    reporter.result(failed);

    REQUIRE(!reporter.finish());

    // twice, and deliberately: the line is where a reader watching the run sees it, the block under the
    // summary is where a reader of a finished run looks
    REQUIRE(out.str() ==
        "mylib/math.eco\n"
        "  ✗ subtracts                                           4 ms  exited 1\n"
        "      assertion failed: nope\n"
        "  1 test, 4 ms\n"
        "\n"
        "1 test, 1 failed\n"
        "\n✗ mylib/math.eco::subtracts  exited 1\n"
        "    assertion failed: nope\n");
}

TEST_CASE("a listing names a test's group where it has one", "[testreporter]")
{
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal(), TestDetail::t_listing);

    TestResult grouped = a_result("src/math.eco", "adds_up", 1);
    grouped.test.group = "arithmetic";

    reporter.begin(1);
    reporter.result(grouped);
    reporter.finish();

    REQUIRE(out.str().find("  ✓ adds_up                                             1 ms  (arithmetic)\n")
        != std::string::npos);
}

TEST_CASE("both themes list the same information", "[testreporter]")
{
    auto listing = [](bool unicode) {
        std::ostringstream out;
        ProgressReporter progress;
        TestReporter reporter(out, progress, a_terminal(unicode), TestDetail::t_listing);

        reporter.begin(2);
        reporter.result(a_result("src/math.eco", "adds_up", 3));
        reporter.result(a_result("src/math.eco", "subtracts", 1, TestOutcome::t_failed));
        reporter.finish();

        return out.str();
    };

    const std::string pretty = listing(true);
    const std::string ascii = listing(false);

    REQUIRE(pretty != ascii);

    // the marks differ and nothing else does - the same names, the same columns, the same numbers
    for (const char *word : { "mylib/math.eco", "adds_up", "3 ms", "subtracts", "exited 1", "2 tests, 4 ms" }) {
        REQUIRE(pretty.find(word) != std::string::npos);
        REQUIRE(ascii.find(word) != std::string::npos);
    }

    REQUIRE(pretty.find(PASS) != std::string::npos);
    REQUIRE(ascii.find(PASS) == std::string::npos);
    REQUIRE(ascii.find("  + adds_up") != std::string::npos);
    REQUIRE(ascii.find("  x subtracts") != std::string::npos);
}

TEST_CASE("colour wraps a listing's mark and nothing that is measured", "[testreporter]")
{
    std::ostringstream out;
    ProgressReporter progress;
    TestReporter reporter(out, progress, a_terminal(true, 80, /*color=*/true), TestDetail::t_listing);

    reporter.begin(1);
    reporter.result(a_result("src/math.eco", "adds_up", 3));
    reporter.finish();

    // the escapes sit either side of the glyph, so the name still starts at the column it does without them
    REQUIRE(out.str().find(std::string("  \x1b[1;32m") + PASS + "\x1b[0m adds_up") != std::string::npos);
    REQUIRE(out.str().find("3 ms\x1b[") == std::string::npos);
}

TEST_CASE("a listing writes no cursor movement, terminal or not", "[testreporter]")
{
    // the assertion that the checklist really was stood down rather than written over. A live row and a
    // listing on one stream is the two-writers problem Compiler::ProgressReporter exists to prevent
    std::ostringstream progress_out;
    std::ostringstream out;

    ProgressReporter progress(progress_out, a_terminal());
    TestReporter reporter(out, progress, a_terminal(), TestDetail::t_listing);

    reporter.begin(1);
    reporter.result(a_result("src/math.eco", "adds_up", 3));

    REQUIRE(reporter.finish());

    REQUIRE(out.str().find('\r') == std::string::npos);
    REQUIRE(out.str().find("\x1b[K") == std::string::npos);

    // nothing was drawn on the checklist's own stream either: a listing opens no row, so there is none to
    // erase and no closing line owed
    REQUIRE(progress_out.str() == "");
}

TEST_CASE("a live counter still commits a failure row and closes itself", "[testreporter]")
{
    // the other half of the same split: with the default detail level a terminal gets the checklist, and
    // that is the rendering --verbose replaces rather than decorates
    std::ostringstream progress_out;
    std::ostringstream out;

    ProgressReporter progress(progress_out, a_terminal());
    TestReporter reporter(out, progress, a_terminal());

    reporter.begin(1);
    reporter.result(a_result("src/math.eco", "subtracts", 1, TestOutcome::t_failed));

    REQUIRE(!reporter.finish());

    REQUIRE(progress_out.str().find("subtracts") != std::string::npos);
    REQUIRE(progress_out.str().find("1 test, 1 failed") != std::string::npos);

    // and the results stream carries no per-test line at all - that is the checklist's job here
    REQUIRE(out.str().find("ok ") == std::string::npos);
}
