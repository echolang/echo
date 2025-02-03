#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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
// are matched the same way - the compiler prints its diagnostics to stdout, so an error
// test's golden simply contains the expected diagnostic text
//
// the corpus is data driven: adding `.eco`/`.eco.out` pairs (in arbitrarily nested
// subdirs) needs no CMake reconfigure - discovery happens at runtime

#ifndef ECHOC_BINARY
#define ECHOC_BINARY "echoc"
#endif

#ifndef ECO_E2E_TESTS_DIR
#define ECO_E2E_TESTS_DIR "tests_eco"
#endif

namespace fs = std::filesystem;

namespace
{
    std::string read_file(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // leading and trailing whitespace stripped, "" when there is nothing else. one answer for the
    // flags file and for a directive line, which are read the same way and must not differ on
    // whether an editor's trailing newline counts
    std::string trim(const std::string &s)
    {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }

    // the extra flags a test asks for, from a sibling `<name>.eco.flags`, or "" when there is none
    //
    // most tests need none - the point of a golden is that the same invocation produces the same
    // output. but a flag can be the thing under test: `--release` is what makes an `assert`
    // disappear, and no amount of source can show that from inside the program
    std::string read_flags(const fs::path &eco)
    {
        fs::path flags_path = eco;
        flags_path += ".flags";

        if (!fs::exists(flags_path)) {
            return "";
        }

        // one line, whitespace-trimmed - a trailing newline from an editor must not become an
        // empty argument
        return trim(read_file(flags_path));
    }

    // run `echoc run [extra] [flags] <file>` capturing merged stdout+stderr, return the captured text.
    // 2>&1 so a rare stderr line (e.g. "No source files") is captured too; in practice a
    // successful run prints only program output and a failing run prints only diagnostics
    //
    // `extra` is the caller's own flag rather than the test's - `--print-ir` is the only one, and it
    // goes through the same invocation so an IR check is about the code that actually ran
    std::string run_echoc(const fs::path &eco, const std::string &extra = "")
    {
        const std::string flags = read_flags(eco);

        std::string cmd = "\"" ECHOC_BINARY "\" run "
            + (extra.empty() ? "" : extra + " ")
            + (flags.empty() ? "" : flags + " ")
            + "\"" + eco.string() + "\" 2>&1";

        std::string out;
        std::array<char, 4096> buf;
        FILE *pipe = popen(cmd.c_str(), "r");
        REQUIRE(pipe != nullptr);

        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
            out.append(buf.data(), n);
        }

        // exit code is intentionally ignored - the captured output is the contract
        pclose(pipe);
        return out;
    }

    // strip a single trailing newline so goldens don't have to be byte-perfect on the last \n
    std::string normalize(std::string s)
    {
        if (!s.empty() && s.back() == '\n') {
            s.pop_back();
        }
        return s;
    }

    // --- generated-IR checks ---------------------------------------------------------------------
    //
    // a sibling `<name>.eco.ir` asserts things about the *emitted LLVM IR* that no amount of program
    // output can show: that a literal is a constant rather than an allocation, that `echo` of a string
    // reaches `write` rather than `printf`, that passing a view costs no reference count.
    //
    // **directives, not a byte-for-byte golden**, and that is deliberate. the IR carries a
    // `target triple` and a `target datalayout` that are machine specific, `attributes #0` that depend
    // on the LLVM version, and `%0`/`%1` numbering that shifts with any unrelated codegen change. a
    // full-text golden would therefore fail on every machine but one and churn on every commit - and a
    // golden that is regenerated without being read asserts nothing at all.
    //
    // the format is LLVM's own FileCheck, reduced to the two directives that carry their weight:
    //
    //     CHECK:     <substring>   must appear at or after the previous CHECK's match
    //     CHECK-NOT: <substring>   must not appear between the surrounding CHECKs
    //
    // blank lines and `#` / `//` lines are ignored. anything else is an error rather than a no-op: a
    // mistyped directive that silently checks nothing is the one failure mode this must not have.
    struct IrDirective
    {
        bool negated;
        std::string text;
        size_t line;
    };

    // parses the directive file. `out_error` is set (and the result meaningless) on a malformed line
    std::vector<IrDirective> read_ir_directives(const fs::path &ir_file, std::string &out_error)
    {
        std::vector<IrDirective> directives;
        std::istringstream in(read_file(ir_file));

        std::string raw;
        size_t line_number = 0;

        while (std::getline(in, raw)) {
            line_number += 1;
            const std::string line = trim(raw);

            if (line.empty() || line.rfind('#', 0) == 0 || line.rfind("//", 0) == 0) {
                continue;
            }

            // the longer prefix first, or `CHECK-NOT:` would match `CHECK:`'s test and be read as a
            // positive directive whose text happens to start with "-NOT:"
            static constexpr const char *negated_prefix = "CHECK-NOT:";
            static constexpr const char *positive_prefix = "CHECK:";

            const bool negated = line.rfind(negated_prefix, 0) == 0;
            const char *prefix = negated ? negated_prefix
                : (line.rfind(positive_prefix, 0) == 0 ? positive_prefix : nullptr);

            // an unrecognised line is an error rather than a no-op: a mistyped directive that silently
            // checks nothing is the one failure mode this must not have
            if (prefix == nullptr) {
                out_error = ir_file.filename().string() + ":" + std::to_string(line_number)
                    + ": expected 'CHECK:' or 'CHECK-NOT:', got: " + line;
                return directives;
            }

            directives.push_back(IrDirective {
                negated, trim(line.substr(std::strlen(prefix))), line_number });

            if (directives.back().text.empty()) {
                out_error = ir_file.filename().string() + ":" + std::to_string(line_number)
                    + ": directive has no text to match";
                return directives;
            }
        }

        if (directives.empty()) {
            out_error = ir_file.filename().string() + ": no directives - an empty check file asserts nothing";
        }

        return directives;
    }

    // applies the directives to `ir`, returning "" on success or the failure message.
    //
    // a positive CHECK advances a cursor, so ordering is asserted and each CHECK can only match at or
    // after the last one. that is what gives free function scoping - `CHECK: define {{...}} @main`
    // followed by CHECKs that can then only match below it - and it is why a CHECK-NOT is scoped to the
    // region *between* its neighbours rather than to the whole module. whole-module would be useless
    // here: `mem::` declares `malloc`, so "this function does not allocate" has to mean "not in this
    // region", not "nowhere in the program"
    std::string apply_ir_directives(const std::vector<IrDirective> &directives, const std::string &ir)
    {
        size_t cursor = 0;
        std::vector<const IrDirective *> pending_negations;

        auto check_negations = [&](size_t region_end) -> std::string {
            for (const auto *negated : pending_negations) {
                const size_t found = ir.find(negated->text, cursor);

                if (found != std::string::npos && found < region_end) {
                    return "CHECK-NOT on line " + std::to_string(negated->line)
                        + " matched, but must not: " + negated->text;
                }
            }
            pending_negations.clear();
            return "";
        };

        for (const auto &directive : directives) {
            if (directive.negated) {
                pending_negations.push_back(&directive);
                continue;
            }

            const size_t found = ir.find(directive.text, cursor);

            if (found == std::string::npos) {
                return "CHECK on line " + std::to_string(directive.line)
                    + " never matched (searching from offset " + std::to_string(cursor) + "): "
                    + directive.text;
            }

            if (std::string failure = check_negations(found); !failure.empty()) {
                return failure;
            }

            cursor = found + directive.text.size();
        }

        // trailing CHECK-NOTs are scoped to everything after the last positive match
        return check_negations(ir.size());
    }

    // the IR for a test: the same `run` invocation plus `--print-ir`, so the checks are about the code
    // that actually ran. the program's own output follows the IR in this capture and is deliberately
    // left in - the alternative is guessing where the module ends, and a directive only ever matches
    // what it asks for
    std::string run_echoc_ir(const fs::path &eco)
    {
        return run_echoc(eco, "--print-ir");
    }
};

TEST_CASE("eco end-to-end golden output", "[e2e]")
{
    const fs::path root = ECO_E2E_TESTS_DIR;

    std::vector<fs::path> cases;
    for (auto &entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".eco") {
            cases.push_back(entry.path());
        }
    }

    // deterministic order across platforms
    std::sort(cases.begin(), cases.end());

    REQUIRE_FALSE(cases.empty());

    for (const auto &eco : cases) {
        fs::path expected = eco;
        expected += ".out";
        std::string rel = fs::relative(eco, root).string();

        DYNAMIC_SECTION("eco: " << rel)
        {
            if (!fs::exists(expected)) {
                FAIL("missing golden file: " << rel << ".out, create it with: ./build/echoc run \"" << eco.string() << "\" > \"" << expected.string() << "\"");
            }

            std::string actual = normalize(run_echoc(eco));
            std::string want = normalize(read_file(expected));

            INFO("file:     " << eco.string());
            INFO("expected:\n" << want);
            INFO("actual:\n" << actual);
            CHECK(actual == want);
        }

        // the optional second half: what the *emitted IR* must look like. a separate section so a
        // failure names which of the two contracts broke, and a separate `echoc` run so `--print-ir`
        // can never pollute the output golden above
        fs::path ir_checks = eco;
        ir_checks += ".ir";

        if (!fs::exists(ir_checks)) {
            continue;
        }

        DYNAMIC_SECTION("ir: " << rel)
        {
            std::string parse_error;
            const auto directives = read_ir_directives(ir_checks, parse_error);

            INFO("file: " << eco.string());

            if (!parse_error.empty()) {
                FAIL(parse_error);
            }

            const std::string ir = run_echoc_ir(eco);
            const std::string failure = apply_ir_directives(directives, ir);

            INFO("ir:\n" << ir);
            CHECK(failure == "");
        }
    }
}
