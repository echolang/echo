#include <catch2/catch_test_macros.hpp>

#include <Compiler/AbortReport.h>

#include <optional>
#include <string>

using Compiler::parse_abort_report;

TEST_CASE("parse_abort_report reads write_pieces", "[abort_report]")
{
    const auto report = parse_abort_report(
        "fatal error: nothing to do here\n"
        "  at a_die_is_a_failure.eco:4\n");

    REQUIRE(report.has_value());
    REQUIRE(report->headline == "fatal error");
    REQUIRE(report->message == "nothing to do here");
    REQUIRE(report->file == "a_die_is_a_failure.eco");
    REQUIRE(report->line == 4);
}

TEST_CASE("parse_abort_report splits assertion failed", "[abort_report]")
{
    const auto report = parse_abort_report(
        "assertion failed: one is not two\n"
        "  at math.eco:16\n");

    REQUIRE(report.has_value());
    REQUIRE(report->headline == "assertion failed");
    REQUIRE(report->message == "one is not two");
    REQUIRE(report->file == "math.eco");
    REQUIRE(report->line == 16);
}

TEST_CASE("parse_abort_report keeps extra message lines", "[abort_report]")
{
    const auto report = parse_abort_report(
        "fatal error: first line\n"
        "second line\n"
        "  at die.eco:9\n");

    REQUIRE(report.has_value());
    REQUIRE(report->headline == "fatal error");
    REQUIRE(report->message == "first line\nsecond line");
    REQUIRE(report->file == "die.eco");
    REQUIRE(report->line == 9);
}

TEST_CASE("parse_abort_report accepts a headline with no message", "[abort_report]")
{
    const auto report = parse_abort_report(
        "fatal error\n"
        "  at x.eco:1\n");

    REQUIRE(report.has_value());
    REQUIRE(report->headline == "fatal error");
    REQUIRE(report->message.empty());
    REQUIRE(report->file == "x.eco");
    REQUIRE(report->line == 1);
}

TEST_CASE("parse_abort_report ignores trailing lines after the location", "[abort_report]")
{
    const auto report = parse_abort_report(
        "fatal error: outer\n"
        "  at test.eco:9\n"
        "gate log flushed\n");

    REQUIRE(report.has_value());
    REQUIRE(report->headline == "fatal error");
    REQUIRE(report->message == "outer");
    REQUIRE(report->file == "test.eco");
    REQUIRE(report->line == 9);
}

TEST_CASE("parse_abort_report reads a drive-letter file from the last colon", "[abort_report]")
{
    const auto report = parse_abort_report(
        "fatal error: nope\n"
        "  at C:\\work\\app.eco:12\n");

    REQUIRE(report.has_value());
    REQUIRE(report->file == "C:\\work\\app.eco");
    REQUIRE(report->line == 12);
}

TEST_CASE("parse_abort_report is empty without a location line", "[abort_report]")
{
    REQUIRE_FALSE(parse_abort_report("fatal error: nope\n").has_value());
    REQUIRE_FALSE(parse_abort_report("").has_value());
    REQUIRE_FALSE(parse_abort_report("  at missing-line\n").has_value());
}
