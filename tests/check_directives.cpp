#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "eco_check_directives.h"

using EchoTests::CheckDirective;
using EchoTests::apply_check_directives;
using EchoTests::parse_check_directives;

namespace
{
    std::vector<CheckDirective> parse_or_fail(const std::string &body)
    {
        std::vector<CheckDirective> directives;
        std::string error;
        REQUIRE(parse_check_directives(body, 1, "t.test", directives, error));
        REQUIRE(error.empty());
        return directives;
    }
};

TEST_CASE("host-gated CHECK is skipped on other hosts", "[check]")
{
    const auto directives = parse_or_fail(
        "CHECK: start\n"
        "CHECK-WINDOWS: win-only\n"
        "CHECK: end\n");

    REQUIRE(directives.size() == 3);
    CHECK(directives[0].host_os.empty());
    CHECK(directives[1].host_os == "windows");
    CHECK_FALSE(directives[1].negated);
    CHECK(directives[2].host_os.empty());

    const std::string both = "start\nend\n";
    CHECK(apply_check_directives(directives, both, "linux") == "");
    CHECK(apply_check_directives(directives, both, "windows").find("never matched")
        != std::string::npos);

    const std::string windows = "start\nwin-only\nend\n";
    CHECK(apply_check_directives(directives, windows, "windows") == "");
    CHECK(apply_check_directives(directives, windows, "linux") == "");
}

TEST_CASE("host-gated CHECK-NOT is skipped on other hosts", "[check]")
{
    const auto directives = parse_or_fail(
        "CHECK: start\n"
        "CHECK-NOT-WINDOWS: forbidden\n"
        "CHECK: end\n");

    REQUIRE(directives.size() == 3);
    CHECK(directives[1].host_os == "windows");
    CHECK(directives[1].negated);

    const std::string hay = "start\nforbidden\nend\n";
    CHECK(apply_check_directives(directives, hay, "linux") == "");
    CHECK(apply_check_directives(directives, hay, "windows").find("matched, but must not")
        != std::string::npos);
}

TEST_CASE("unknown host in a CHECK is an error", "[check]")
{
    std::vector<CheckDirective> directives;
    std::string error;
    REQUIRE_FALSE(parse_check_directives("CHECK-FREEBSD: foo\n", 1, "t.test", directives, error));
    CHECK(error.find("unknown host 'freebsd'") != std::string::npos);
}

TEST_CASE("CHECK-NOT still beats CHECK as a prefix", "[check]")
{
    const auto directives = parse_or_fail("CHECK-NOT: absent\nCHECK: present\n");
    REQUIRE(directives.size() == 2);
    CHECK(directives[0].negated);
    CHECK(directives[0].host_os.empty());
    CHECK_FALSE(directives[1].negated);
}
