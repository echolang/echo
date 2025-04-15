#include <catch2/catch_test_macros.hpp>

#include "eco_test_file.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <vector>

// end-to-end suite
//
// discovers every `*.eco` under ECO_E2E_TESTS_DIR recursively and reads its sibling `*.test`, which
// is the whole contract for that case: the settings it runs under, the output it must print, and
// optionally what the emitted IR or either AST dump must contain. see tests_eco/README.md for the
// format; EchoTests::parse_eco_test_file owns it.
//
// the corpus is data driven: adding a `.eco`/`.test` pair (in arbitrarily nested subdirs) needs no
// CMake reconfigure - discovery happens at runtime

#ifndef ECHOC_BINARY
#define ECHOC_BINARY "echoc"
#endif

#ifndef ECO_E2E_TESTS_DIR
#define ECO_E2E_TESTS_DIR "tests_eco"
#endif

#ifndef ECO_E2E_TMP_DIR
#define ECO_E2E_TMP_DIR "e2e_tmp"
#endif

namespace fs = std::filesystem;

namespace
{
    // the exit status matters now: `expect` is what makes a deliberately broken program's rejection
    // part of the contract, rather than a coincidence of the text it happened to print
    struct ProcessResult
    {
        int exit_code = 0;
        std::string output;
    };

    // runs a shell command capturing merged stdout+stderr.
    //
    // one function rather than a popen block per call site, so the wait status is decoded in exactly
    // one place - and so a future `timeout:` setting has one place to go. a signal is reported as
    // `128 + signo`, the way a shell reports it, which keeps a JIT segfault distinguishable from a
    // clean rejection in the failure message.
    //
    // a failure to spawn is reported through `exit_code` like any other, never with a Catch2
    // assertion: this is the suite's process primitive, and a primitive that asserts can only be
    // called from inside an assertion context - which a cached probe run at static-init time is not
    ProcessResult run_capturing(const std::string &command)
    {
        ProcessResult result;

        std::array<char, 4096> buf;
        FILE *pipe = popen(command.c_str(), "r");

        if (!pipe) {
            result.exit_code = 127;
            result.output = "could not spawn: " + command;
            return result;
        }

        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
            result.output.append(buf.data(), n);
        }

        const int status = pclose(pipe);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

        return result;
    }

    std::string quoted(const fs::path &p)
    {
        return "\"" + p.string() + "\"";
    }

    // `echoc <run|build -o bin> [dump] [flags] <file>`.
    //
    // one builder for both subcommands: they differ in the subcommand word and the `-o` target and in
    // nothing else, and as two functions the dump splice, the flags and the redirect were written
    // twice. `dump` is empty for the output run - the OUT golden is a byte comparison, so nothing may
    // be added to that stream.
    //
    // which subcommand it is comes off `test.mode` and not off whether a binary was handed in: those
    // were two encodings of one fact, and passing null for a `mode: build` case silently produced a
    // *run* invocation that nothing in here would have noticed
    std::string echoc_command(
        const EchoTests::EcoTestFile &test, const fs::path &eco, const fs::path *binary,
        const std::string &dump)
    {
        const bool is_build = test.mode == EchoTests::RunMode::t_build;

        REQUIRE((binary != nullptr) == is_build);

        return "\"" ECHOC_BINARY "\" "
            + (is_build ? "build -o " + quoted(*binary) + " " : std::string("run "))
            + (dump.empty() ? "" : dump + " ")
            + test.compiler_flags()
            + quoted(eco) + " 2>&1";
    }

    // where a `mode: build` case's binary goes: the build tree, absolute.
    //
    // absolute because the tests binary's working directory is not fixed - CI runs `./tests` from
    // inside `build/`, a developer runs `./build/tests` from the repo root - so any relative path is
    // wrong in one of the two, and a repo-root-relative one drops artifacts among tracked files
    fs::path temp_binary_for(const fs::path &eco, const fs::path &root)
    {
        std::string name = fs::relative(eco, root).replace_extension("").string();
        std::replace(name.begin(), name.end(), '/', '_');
        std::replace(name.begin(), name.end(), '\\', '_');

        return fs::path(ECO_E2E_TMP_DIR) / name;
    }

    // make_exec leaves two artifacts, the executable and `<executable>.o`. both go, always,
    // including after a failure - Catch2's INFO has already captured everything a human needs by
    // then, and a stale binary from a previous run is a false pass waiting to happen
    struct ScopedBinary
    {
        fs::path path;

        ScopedBinary(fs::path p) : path(std::move(p))
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            remove_artifacts();
        }

        ~ScopedBinary()
        {
            remove_artifacts();
        }

        void remove_artifacts()
        {
            std::error_code ec;
            fs::remove(path, ec);
            fs::remove(path.string() + ".o", ec);
        }
    };

    // `clang` is what Backend::make_exec shells out to link with. a FAIL rather than a skip: a suite
    // that quietly stops testing native builds is exactly the silent no-op this corpus refuses
    void require_clang()
    {
        // through the suite's one process primitive, so the wait status is decoded where every other
        // spawn's is - and a spawn failure reads as an exit code here too rather than as a crash
        static const bool available = run_capturing("command -v clang").exit_code == 0;

        if (!available) {
            FAIL("a 'mode: build' test needs `clang` on PATH - echoc links the emitted object with it");
        }
    }

    // did the process the case says owns its status exit the way the case expects? answers so, having
    // reported the mismatch, so that one message shape serves both callers while each still decides
    // what a mismatch costs it - the golden path has already compared its output and carries on, the
    // dump path has nothing worth asserting against a stream that was never produced
    bool check_status(
        const EchoTests::EcoTestFile &test, const ProcessResult &result, const char *actor)
    {
        if (EchoTests::status_matches(test.expect, result.exit_code)) {
            return true;
        }

        FAIL_CHECK("expected this case to " << EchoTests::expectation_name(test.expect)
            << ", but " << actor << " exited " << result.exit_code);

        return false;
    }

    // asserts one dump section. its own invocation, and its own Catch2 section, so a failure names
    // which of the case's contracts broke.
    //
    // the dumps cannot share one invocation: `-a` and `-ar` print byte-identical module headers, so
    // there would be nothing to split a combined stream on, and on a rejected program `-p` never
    // runs at all
    void check_dump_section(
        const EchoTests::EcoTestFile &test, const EchoTests::CheckSection &section,
        const fs::path &eco, const fs::path &root)
    {
        const std::string flag = EchoTests::dump_flag(section.kind);

        // the binary is what a build-mode dump invocation owes `-o`, and nothing here reads it. one
        // invocation rather than one per mode: the subcommand is echoc_command's to decide
        std::optional<ScopedBinary> binary;

        if (test.mode == EchoTests::RunMode::t_build) {
            require_clang();
            binary.emplace(temp_binary_for(eco, root));
        }

        const ProcessResult result = run_capturing(
            echoc_command(test, eco, binary.has_value() ? &binary->path : nullptr, flag));

        INFO("dump:\n" << result.output);

        // the status is asserted before the directives, and on this path too - a dump invocation that
        // died early (a mistyped `flags:` argparse refuses, a crash before the dump runs) produces a
        // short or empty stream, and CHECK-NOTs against a stream that was never produced all hold
        if (!check_status(test, result, "the dump invocation")) {
            return;
        }

        CHECK(EchoTests::apply_check_directives(section.directives, result.output) == "");
    }

    // the OUT golden plus the exit status, for whichever process the case says owns its output.
    // `actor` is the only thing that differs between the two modes, so the contract - what counts as
    // matching, and what a failure shows - is spelled once
    void check_program_output(
        const EchoTests::EcoTestFile &test, const ProcessResult &result, const char *actor)
    {
        const std::string actual = EchoTests::strip_trailing_newline(result.output);

        INFO("expected:\n" << test.expected_output);
        INFO("actual:\n" << actual);
        CHECK(actual == test.expected_output);

        check_status(test, result, actor);
    }

    // asserts the OUT golden and the exit status of a `mode: run` case
    void check_run_output(const EchoTests::EcoTestFile &test, const fs::path &eco)
    {
        check_program_output(test, run_capturing(echoc_command(test, eco, nullptr, "")), "echoc");
    }

    // the same for `mode: build`: link a native binary, then run it.
    //
    // the OUT golden is the *program's* output only. the build step is asserted through its exit code
    // alone, because its stdout carries absolute object and executable paths - putting those in a
    // golden would fail on every machine but the one that recorded it, and filtering them would put a
    // copy of the compiler's output format in here
    void check_build_output(
        const EchoTests::EcoTestFile &test, const fs::path &eco, const fs::path &root)
    {
        require_clang();

        const ScopedBinary binary(temp_binary_for(eco, root));

        const ProcessResult build = run_capturing(echoc_command(test, eco, &binary.path, ""));

        if (build.exit_code != 0) {
            INFO("build log:\n" << build.output);

            // an `expect: fail` case that never linked is still owed its golden, against the build
            // log - the OUT section is mandatory precisely so that no combination of settings makes
            // it optional again, and a bare `SUCCEED()` here made "rejected somehow" the whole
            // assertion for every build-mode case rejected at compile time
            if (test.expect == EchoTests::Expectation::t_fail) {
                check_program_output(test, build, "echoc build");
                return;
            }

            FAIL("echoc build exited " << build.exit_code);
        }

        // make_exec used to report all three of its failure paths by printing and returning, so
        // "exited 0" is on its own no proof that anything was linked
        if (!fs::exists(binary.path)) {
            INFO("build log:\n" << build.output);
            FAIL("echoc build exited 0 but produced no binary at " << binary.path.string());
        }

        check_program_output(test, run_capturing(quoted(binary.path) + " 2>&1"), "the program");
    }

    // one discovered case: the program, its parsed contract, and whether there was one to parse
    struct DiscoveredCase
    {
        fs::path eco;
        std::string rel;            // relative to the corpus root, for the section name
        bool has_test_file = false;
        EchoTests::EcoTestFile test;
        std::string parse_error;
    };

    struct Corpus
    {
        std::vector<DiscoveredCase> cases;
        std::vector<fs::path> orphans;   // a `.test` with no `.eco` beside it
    };

    Corpus discover_corpus(const fs::path &root)
    {
        Corpus corpus;
        std::vector<fs::path> eco_files;
        std::vector<fs::path> test_files;

        for (auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            if (entry.path().extension() == ".eco") {
                eco_files.push_back(entry.path());
            }
            else if (entry.path().extension() == ".test") {
                test_files.push_back(entry.path());
            }
        }

        // deterministic order across platforms
        std::sort(eco_files.begin(), eco_files.end());
        std::sort(test_files.begin(), test_files.end());

        for (const auto &eco : eco_files) {
            fs::path test_path = eco;
            test_path.replace_extension(".test");

            DiscoveredCase discovered;
            discovered.eco = eco;
            discovered.rel = fs::relative(eco, root).string();
            discovered.has_test_file = fs::exists(test_path);

            if (discovered.has_test_file
                && !EchoTests::parse_eco_test_file(test_path, discovered.test, discovered.parse_error)) {
                discovered.test.checks.clear();
            }

            corpus.cases.push_back(std::move(discovered));
        }

        // a `.test` whose program was renamed away asserts nothing and is invisible to the loop above,
        // so it is collected from the other side
        for (const auto &test_file : test_files) {
            fs::path eco = test_file;
            eco.replace_extension(".eco");

            if (!fs::exists(eco)) {
                corpus.orphans.push_back(test_file);
            }
        }

        return corpus;
    }

    // discovered and parsed **once** per process, not once per section.
    //
    // Catch2 re-runs a TEST_CASE body from the top for every leaf section it enters, so anything in
    // that body happens once per case in the corpus *times* the number of sections. the walk and the
    // ~200 `.test` parses that produce this list are what the sections are derived *from*, which is
    // why they cannot move inside one - so they are cached instead, the way require_clang's probe is
    const Corpus &corpus()
    {
        static const Corpus discovered = discover_corpus(ECO_E2E_TESTS_DIR);

        return discovered;
    }
};

TEST_CASE("eco end-to-end", "[e2e]")
{
    const fs::path root = ECO_E2E_TESTS_DIR;
    const Corpus &discovered = corpus();

    REQUIRE_FALSE(discovered.cases.empty());

    // one section for all of them rather than one each: an orphan is named by its own CHECK, and 200
    // sections that do nothing but an fs::exists double how many times Catch2 re-enters this body
    SECTION("orphan test files")
    {
        for (const auto &orphan : discovered.orphans) {
            FAIL_CHECK("no program next to this test file, expected "
                << fs::path(orphan).replace_extension(".eco").string());
        }
    }

    for (const auto &entry : discovered.cases) {
        DYNAMIC_SECTION("eco: " << entry.rel)
        {
            INFO("file: " << entry.eco.string());

            if (!entry.has_test_file) {
                FAIL("missing test file: " << fs::path(entry.rel).replace_extension(".test").string()
                    << ", see tests_eco/README.md for the format");
            }

            if (!entry.parse_error.empty()) {
                FAIL(entry.parse_error);
            }

            if (entry.test.mode == EchoTests::RunMode::t_build) {
                check_build_output(entry.test, entry.eco, root);
            }
            else {
                check_run_output(entry.test, entry.eco);
            }
        }

        // the optional other half: what the emitted IR or either AST dump must contain. empty for a
        // case that did not parse, so a broken `.test` reports its parse error once and not per section
        for (const auto &section : entry.test.checks) {
            DYNAMIC_SECTION(EchoTests::dump_section_name(section.kind) << ": " << entry.rel)
            {
                INFO("file: " << entry.eco.string());
                check_dump_section(entry.test, section, entry.eco, root);
            }
        }
    }
}
