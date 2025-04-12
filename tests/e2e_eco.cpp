#include <catch2/catch_test_macros.hpp>

#include "eco_test_file.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
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

    // `echoc run [dump] [flags] <file>`. `dump` is empty for the output run - the OUT golden is a
    // byte comparison, so nothing may be added to that stream
    std::string run_command(
        const EchoTests::EcoTestFile &test, const fs::path &eco, const std::string &dump)
    {
        return "\"" ECHOC_BINARY "\" run "
            + (dump.empty() ? "" : dump + " ")
            + test.compiler_flags()
            + quoted(eco) + " 2>&1";
    }

    std::string build_command(
        const EchoTests::EcoTestFile &test, const fs::path &eco, const fs::path &binary,
        const std::string &dump)
    {
        return "\"" ECHOC_BINARY "\" build -o " + quoted(binary) + " "
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
        static const bool available = std::system("command -v clang > /dev/null 2>&1") == 0;

        if (!available) {
            FAIL("a 'mode: build' test needs `clang` on PATH - echoc links the emitted object with it");
        }
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

        ProcessResult result;
        if (test.mode == EchoTests::RunMode::t_build) {
            require_clang();
            const ScopedBinary binary(temp_binary_for(eco, root));
            result = run_capturing(build_command(test, eco, binary.path, flag));
        }
        else {
            result = run_capturing(run_command(test, eco, flag));
        }

        INFO("dump:\n" << result.output);

        // the status is asserted before the directives, and on this path too - a dump invocation that
        // died early (a mistyped `flags:` argparse refuses, a crash before the dump runs) produces a
        // short or empty stream, and CHECK-NOTs against a stream that was never produced all hold
        if (!EchoTests::status_matches(test.expect, result.exit_code)) {
            FAIL("expected this case to " << EchoTests::expectation_name(test.expect)
                << ", but the dump invocation exited " << result.exit_code);
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

        if (!EchoTests::status_matches(test.expect, result.exit_code)) {
            FAIL_CHECK("expected this case to " << EchoTests::expectation_name(test.expect)
                << ", but " << actor << " exited " << result.exit_code);
        }
    }

    // asserts the OUT golden and the exit status of a `mode: run` case
    void check_run_output(const EchoTests::EcoTestFile &test, const fs::path &eco)
    {
        check_program_output(test, run_capturing(run_command(test, eco, "")), "echoc");
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

        const ProcessResult build = run_capturing(build_command(test, eco, binary.path, ""));

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
};

TEST_CASE("eco end-to-end", "[e2e]")
{
    const fs::path root = ECO_E2E_TESTS_DIR;

    std::vector<fs::path> cases;
    std::vector<fs::path> test_files;

    for (auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() == ".eco") {
            cases.push_back(entry.path());
        }
        else if (entry.path().extension() == ".test") {
            test_files.push_back(entry.path());
        }
    }

    // deterministic order across platforms
    std::sort(cases.begin(), cases.end());
    std::sort(test_files.begin(), test_files.end());

    REQUIRE_FALSE(cases.empty());

    // a `.test` whose program was renamed away asserts nothing and is invisible to the loop below,
    // so it is checked from the other side
    for (const auto &test_file : test_files) {
        fs::path eco = test_file;
        eco.replace_extension(".eco");

        DYNAMIC_SECTION("orphan: " << fs::relative(test_file, root).string())
        {
            if (!fs::exists(eco)) {
                FAIL("no program next to this test file, expected " << eco.string());
            }
        }
    }

    for (const auto &eco : cases) {
        fs::path test_path = eco;
        test_path.replace_extension(".test");

        const std::string rel = fs::relative(eco, root).string();

        EchoTests::EcoTestFile test;
        std::string parse_error;

        // parsed once, outside the sections, because the section list below is derived from it
        const bool exists = fs::exists(test_path);
        const bool parsed = exists && EchoTests::parse_eco_test_file(test_path, test, parse_error);

        DYNAMIC_SECTION("eco: " << rel)
        {
            INFO("file: " << eco.string());

            if (!exists) {
                FAIL("missing test file: " << fs::relative(test_path, root).string()
                    << ", see tests_eco/README.md for the format");
            }

            if (!parsed) {
                FAIL(parse_error);
            }

            if (test.mode == EchoTests::RunMode::t_build) {
                check_build_output(test, eco, root);
            }
            else {
                check_run_output(test, eco);
            }
        }

        if (!parsed) {
            continue;
        }

        // the optional other half: what the emitted IR or either AST dump must contain
        for (const auto &section : test.checks) {
            DYNAMIC_SECTION(EchoTests::dump_section_name(section.kind) << ": " << rel)
            {
                INFO("file: " << eco.string());
                check_dump_section(test, section, eco, root);
            }
        }
    }
}
