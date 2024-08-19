#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// end-to-end golden-output suite
//
// discovers every `*.eco` under ECO_E2E_TESTS_DIR recursively, runs it through the
// real `echoc run` binary (ECHOC_BINARY), captures stdout+stderr and compares it to the
// sibling `*.eco.out` golden file. both successful programs and deliberately broken ones
// are matched the same way — the compiler prints its diagnostics to stdout, so an error
// test's golden simply contains the expected diagnostic text.
//
// the corpus is data driven: adding `.eco`/`.eco.out` pairs (in arbitrarily nested
// subdirs) needs no CMake reconfigure — discovery happens at runtime.

#ifndef ECHOC_BINARY
#define ECHOC_BINARY "echoc"
#endif

#ifndef ECO_E2E_TESTS_DIR
#define ECO_E2E_TESTS_DIR "tests_eco"
#endif

namespace fs = std::filesystem;

namespace
{
    // run `echoc run <file>` capturing merged stdout+stderr, return the captured text.
    // 2>&1 so a rare stderr line (e.g. "No source files") is captured too; in practice a
    // successful run prints only program output and a failing run prints only diagnostics
    std::string run_echoc(const fs::path &eco)
    {
        std::string cmd = "\"" ECHOC_BINARY "\" run \"" + eco.string() + "\" 2>&1";

        std::string out;
        std::array<char, 4096> buf;
        FILE *pipe = popen(cmd.c_str(), "r");
        REQUIRE(pipe != nullptr);

        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
        {
            out.append(buf.data(), n);
        }

        // exit code is intentionally ignored — the captured output is the contract
        pclose(pipe);
        return out;
    }

    std::string read_file(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // strip a single trailing newline so goldens don't have to be byte-perfect on the last \n
    std::string normalize(std::string s)
    {
        if (!s.empty() && s.back() == '\n')
        {
            s.pop_back();
        }
        return s;
    }
};

TEST_CASE("eco end-to-end golden output", "[e2e]")
{
    const fs::path root = ECO_E2E_TESTS_DIR;

    std::vector<fs::path> cases;
    for (auto &entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".eco")
        {
            cases.push_back(entry.path());
        }
    }

    // deterministic order across platforms
    std::sort(cases.begin(), cases.end());

    REQUIRE_FALSE(cases.empty());

    for (const auto &eco : cases)
    {
        fs::path expected = eco;
        expected += ".out";
        std::string rel = fs::relative(eco, root).string();

        DYNAMIC_SECTION("eco: " << rel)
        {
            if (!fs::exists(expected))
            {
                FAIL("missing golden file: " << rel << ".out — create it with: ./build/echoc run \"" << eco.string() << "\" > \"" << expected.string() << "\"");
            }

            std::string actual = normalize(run_echoc(eco));
            std::string want = normalize(read_file(expected));

            INFO("file:     " << eco.string());
            INFO("expected:\n" << want);
            INFO("actual:\n" << actual);
            CHECK(actual == want);
        }
    }
}
