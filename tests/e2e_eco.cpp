#include <catch2/catch_test_macros.hpp>

#include "Compiler/HostTool.h"
#include "eco_test_file.h"
#include "subprocess.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
    // the shared process primitive - see subprocess.h
    using EchoTests::ProcessResult;
    using EchoTests::quoted;

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
    // **everything one case writes goes into one directory of its own, inside the build tree** - the
    // `-o` binary, the per-module objects echoc emits beside it, and the module cache. This function is
    // the only thing that says where that is, so nothing has to agree with a second spelling of it.
    //
    // one directory *per case* rather than one shared tree, for two reasons. A case that passes only
    // because an earlier case populated something is not a test - and per case is what makes the order
    // cases run in irrelevant, which is what lets Catch2 shuffle them. It is also what lets the cleanup
    // be a single `remove_all` instead of a guess about how echoc names what it emits.
    //
    // absolute because the tests binary's working directory is not fixed - CI runs `./tests` from inside
    // `build/`, a developer runs `./build/tests` from the repo root - so any relative path is wrong in one
    // of the two, and a repo-root-relative one drops artifacts among tracked files
    fs::path case_scratch_dir(const fs::path &eco, const fs::path &root)
    {
        return fs::path(ECO_E2E_TMP_DIR) / fs::relative(eco, root).replace_extension("");
    }

    std::vector<std::string> echoc_argv(
        const EchoTests::EcoTestFile &test,
        const fs::path &eco,
        const fs::path *binary,
        const std::string &dump,
        const fs::path &root,
        const fs::path &scratch
    )
    {
        const bool is_build = test.mode == EchoTests::RunMode::t_build;

        REQUIRE((binary != nullptr) == is_build);

        std::vector<std::string> argv = { ECHOC_BINARY };

        if (is_build) {
            argv.push_back("build");
            argv.push_back("-o");
            argv.push_back(binary->string());
        }
        else if (test.mode == EchoTests::RunMode::t_test) {
            argv.push_back("test");
        }
        else {
            argv.push_back("run");
        }

        if (!dump.empty()) {
            for (const std::string &word : EchoTests::split_command_words(dump)) {
                argv.push_back(word);
            }
        }

        argv.push_back("--build-dir");
        argv.push_back((scratch / "cache").string());

        for (const std::string &word : EchoTests::split_command_words(test.compiler_flags(root))) {
            argv.push_back(word);
        }

        argv.push_back(eco.string());

        if (!is_build && test.mode != EchoTests::RunMode::t_test && !test.arguments.empty()) {
            argv.push_back("--");
            argv.insert(argv.end(), test.arguments.begin(), test.arguments.end());
        }

        return argv;
    }

    std::vector<std::pair<std::string, std::string>> extra_env_from(const EchoTests::EcoTestFile &test)
    {
        std::vector<std::pair<std::string, std::string>> env;

        for (const std::string &pair : test.environment) {
            const size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                env.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
            }
        }

        return env;
    }

    std::string stdin_text(const EchoTests::EcoTestFile &test)
    {
        std::string text;

        for (const std::string &line : test.stdin_lines) {
            text += line;
            text += '\n';
        }

        return text;
    }

    // brackets a case's scratch directory: empty on the way in, gone on the way out.
    //
    // it goes *always*, including after a failure - Catch2's INFO has already captured everything a
    // human needs by then, and a stale binary or a cached object from a previous run is a false pass
    // waiting to happen. `build` emits one object per module that was not served from the cache, so what
    // is in here is not a fixed list of names; wiping the directory is the only cleanup that cannot
    // fall behind codegen. It is also what keeps the module cache from outliving the compiler that
    // filled it - `compute_module_keys` hashes sources, not the binary that read them
    struct ScopedScratch
    {
        fs::path path;

        ScopedScratch(fs::path p) : path(std::move(p))
        {
            std::error_code ec;
            fs::remove_all(path, ec);
            fs::create_directories(path, ec);
        }

        ~ScopedScratch()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    // the outcome of a case's *spawns*, and nothing else.
    //
    // this is the half of a case that never touches Catch2 - a command to start, what it printed, what
    // it exited with - which is exactly the half worth running on another thread. the assertions that
    // judge it stay on the main thread, in section order, because Catch2's macros are not thread safe.
    // the split is the whole of what makes the corpus parallel: nothing below this struct spawns, and
    // nothing above it asserts
    struct CaseOutcome
    {
        ProcessResult primary;             // `echoc run`, or `echoc build`
        bool binary_exists = false;        // `mode: build` - did the link actually leave one behind
        ProcessResult program;             // the linked binary, when the build got that far
        std::vector<ProcessResult> dumps;  // one per `test.checks`, same order
    };

    std::string describe_exit(int exit_code)
    {
#if defined(_WIN32)
        const auto code = static_cast<unsigned>(exit_code);

        if (code == 0xC0000005u) {
            return "0xC0000005 (access violation)";
        }

        if (code >= 0xC0000000u) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%08X", code);
            return buf;
        }
#endif
        return std::to_string(exit_code);
    }

    // `clang` is what Backend::link_executable falls back to. a FAIL rather than a skip: a suite
    // that quietly stops testing native builds is exactly the silent no-op this corpus refuses
    void require_clang()
    {
        // the same clang echoc itself will invoke, not whatever `where clang` happens to
        // find. PATH often has a GNU-ABI clang first on Windows, and a VS/CMake session
        // may have none at all
        static const bool available = [] {
            const std::string clang = Compiler::host_clang();
            std::error_code ec;
            return std::filesystem::is_regular_file(clang, ec);
        }();

        if (!available) {
            FAIL("a 'mode: build' test needs the host clang echoc was built with ("
                << Compiler::host_clang() << ")");
        }
    }

    // did the process the case says owns its status exit the way the case expects? answers so, having
    // reported the mismatch, so that one message shape serves both callers while each still decides
    // what a mismatch costs it - the golden path has already compared its output and carries on, the
    // dump path has nothing worth asserting against a stream that was never produced
    bool check_status(
        const EchoTests::EcoTestFile &test, const ProcessResult &result, const char *actor)
    {
        if (result.timed_out) {
            FAIL_CHECK(actor << " timed out after the case's deadline");
            return false;
        }

        if (EchoTests::status_matches(test.expect, result.exit_code)) {
            return true;
        }

        FAIL_CHECK("expected this case to " << EchoTests::expectation_name(test.expect)
            << ", but " << actor << " exited " << describe_exit(result.exit_code));

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
        const ProcessResult &result)
    {
        if (test.mode == EchoTests::RunMode::t_build) {
            require_clang();
        }

        INFO("dump:\n" << result.output);

        // the status is asserted before the directives, and on this path too - a dump invocation that
        // died early (a mistyped `flags:` the parser refuses, a crash before the dump runs) produces a
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

    // a `mode: build` case: the build's own status, then the linked program's output.
    //
    // a `mode: run` case needs no counterpart - `echoc run` is the one process, so the judge is
    // check_program_output on the primary spawn and nothing more.
    //
    // the OUT golden is the *program's* output only. the build step is asserted through its exit code
    // alone, because its stdout carries absolute object and executable paths - putting those in a
    // golden would fail on every machine but the one that recorded it, and filtering them would put a
    // copy of the compiler's output format in here
    void check_build_output(
        const EchoTests::EcoTestFile &test, const fs::path &binary, const CaseOutcome &outcome)
    {
        require_clang();

        const ProcessResult &build = outcome.primary;

        if (build.exit_code != 0) {
            INFO("build log:\n" << build.output);

            // an `expect: fail` case that never linked is still owed its golden, against the build
            // log - the OUT section is mandatory precisely so that no combination of settings makes
            // it optional again, and a bare `SUCCEED()` here made "rejected somehow" the whole
            // assertion for every build-mode case rejected at compile time
            // `fail` and not `status_matches`, deliberately: an exact `expect:` pins what the *program*
            // exits with, and a build that never produced one has not met it. Reaching the FAIL below
            // with "echoc build exited 3" is the honest report there, rather than a pass for the right
            // number produced by the wrong process
            if (test.expect.kind == EchoTests::Expectation::Kind::t_fail) {
                check_program_output(test, build, "echoc build");
                return;
            }

            FAIL("echoc build exited " << describe_exit(build.exit_code));
        }

        // the emit path used to report all of its failure paths by printing and returning, so
        // "exited 0" is on its own no proof that anything was linked
        if (!outcome.binary_exists) {
            INFO("build log:\n" << build.output);
            FAIL("echoc build exited 0 but produced no binary at " << binary.string());
        }

        check_program_output(test, outcome.program, "the program");
    }

    // one discovered case: the program, its parsed contract, and whether there was one to parse
    struct DiscoveredCase
    {
        fs::path eco;
        std::string rel;            // relative to the corpus root, for the section name
        bool has_test_file = false;
        EchoTests::EcoTestFile test;
        std::string parse_error;

        // where this case writes, and what it writes there. Both come off case_scratch_dir at discovery,
        // so the `-o` target is decided once: the worker brackets its spawns with the directory, the
        // judge names the binary in a failure, and echoc_command puts both on the command line.
        //
        // `scratch` is empty for a case that is not runnable, `binary` for anything but `mode: build`
        fs::path scratch;
        fs::path binary;

        // is there anything to spawn at all? a case with no `.test`, or one that would not parse, FAILs
        // on that alone - it never reaches an outcome, so nothing should be started for it
        bool runnable() const
        {
            return has_test_file && parse_error.empty();
        }
    };

    struct Corpus
    {
        std::vector<DiscoveredCase> cases;
        std::vector<fs::path> orphans;   // a `.test` with no `.eco` beside it
    };

    // every spawn one case owes, start to finish, with no assertion anywhere in it.
    //
    // the build chain lives here rather than being split across the judge, because the second spawn is
    // conditional on the first: a build that never linked has no program to run. `ScopedScratch` brackets
    // the whole sequence, so the dumps - which reuse the same `-o` target and the same cache - cannot
    // outlive the cleanup, and the program is run before a dump invocation can overwrite the binary
    // underneath it
    CaseOutcome run_case(
        const DiscoveredCase &entry,
        const std::vector<std::string> &command,
        const std::vector<std::vector<std::string>> &dump_commands
    )
    {
        CaseOutcome outcome;
        ScopedScratch scratch(entry.scratch);

        const unsigned deadline = entry.test.timeout_ms;
        const auto env = extra_env_from(entry.test);
        const std::string input = stdin_text(entry.test);

        // stdin belongs to the program, not to `echoc build` or a dump invocation
        outcome.primary = EchoTests::run_process(
            command, deadline, {}, env, entry.binary.empty() ? input : std::string());

        if (!entry.binary.empty()) {
            std::error_code ec;
            outcome.binary_exists = EchoTests::file_exists(entry.binary);

            if (outcome.primary.exit_code == 0 && outcome.binary_exists) {
                // the environment goes on both spawns and the arguments only on this one: a linked binary
                // is the program, so its argv *is* the program's, with no `--` needed to say so
                std::vector<std::string> argv = { EchoTests::with_exe(entry.binary).string() };
                argv.insert(argv.end(), entry.test.arguments.begin(), entry.test.arguments.end());

                outcome.program = EchoTests::run_process(argv, deadline, {}, env, input);
            }
        }

        for (const auto &dump : dump_commands) {
            outcome.dumps.push_back(EchoTests::run_process(dump, deadline, {}, env));
        }

        return outcome;
    }

    // the corpus's outcomes, produced by a pool of workers running a window ahead of whichever case
    // the assertions have reached.
    //
    // **a window rather than all of them up front.** Eager would be three lines shorter and would make
    // `tests "[e2e]" -c "eco: arrays/literals.eco"` pay for the whole corpus; that invocation costs one
    // case today and is the loop a person actually works in. Catch2 enters the sections of a single
    // test case in declaration order - it shuffles test *cases* - so "the next K" is always work that
    // will really be asked for, and a filtered run overruns by at most K.
    //
    // the slots are sized once and never resized, so a reference handed out by `at` stays valid while
    // later cases are still being filled in around it
    class ResultCache
    {
    public:
        ResultCache(const std::vector<DiscoveredCase> &cases, fs::path root)
            : _cases(cases), _root(std::move(root)), _slots(cases.size())
        {
            const unsigned detected = std::thread::hardware_concurrency();
            unsigned workers = detected == 0 ? 4 : detected;

#if defined(_WIN32)
            // each case is its own echoc, and a native build then starts clang. uncapped
            // hardware_concurrency on Windows is enough concurrent compilers to AV a child
            if (workers > 8) {
                workers = 8;
            }
#endif

            for (unsigned i = 0; i < workers; i++) {
                _workers.emplace_back([this] { work(); });
            }
        }

        ~ResultCache()
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _stopping = true;
            }

            _wake_workers.notify_all();

            for (auto &worker : _workers) {
                worker.join();
            }
        }

        const CaseOutcome &at(size_t index)
        {
            std::unique_lock<std::mutex> lock(_mutex);

            // claim this case and the window behind it in one go, so the pool is already busy on what
            // the next sections will ask for by the time this one is judged.
            //
            // the commands are built *here* and not at discovery, for two reasons that point the same
            // way: echoc_command asserts, and only this thread may - and a filtered run should no more
            // pay for 400 commands it will never spawn than for the spawns themselves
            //
            // the window is twice the pool, so every worker has one case in hand and one behind it
            for (size_t i = index; i < _slots.size() && i < index + 2 * _workers.size(); i++) {
                if (_slots[i].state != Slot::State::t_idle) {
                    continue;
                }

                // a case with no `.test`, or one that would not parse, FAILs on that alone - it has
                // nothing to spawn, so it is done the moment it is looked at rather than being queued
                // for a worker to discover it has no command
                if (!_cases[i].runnable()) {
                    _slots[i].state = Slot::State::t_done;
                    continue;
                }

                build_commands(i);

                _slots[i].state = Slot::State::t_claimed;
                _queue.push_back(i);
            }

            _wake_workers.notify_all();
            _slot_done.wait(lock, [&] { return _slots[index].state == Slot::State::t_done; });

            return _slots[index].outcome;
        }

    private:
        struct Slot
        {
            enum class State { t_idle, t_claimed, t_done };

            State state = State::t_idle;
            std::vector<std::string> command;
            std::vector<std::vector<std::string>> dump_commands;
            CaseOutcome outcome;
        };

        // called under the lock, on the main thread only, and for a runnable case only - see `at`
        void build_commands(size_t index)
        {
            const DiscoveredCase &entry = _cases[index];
            const fs::path *binary = entry.binary.empty() ? nullptr : &entry.binary;

            _slots[index].command
                = echoc_argv(entry.test, entry.eco, binary, "", _root, entry.scratch);

            for (const auto &section : entry.test.checks) {
                _slots[index].dump_commands.push_back(echoc_argv(
                    entry.test, entry.eco, binary, EchoTests::dump_flag(section.kind), _root,
                    entry.scratch));
            }
        }

        void work()
        {
            for (;;) {
                size_t index = 0;
                std::vector<std::string> command;
                std::vector<std::vector<std::string>> dump_commands;

                {
                    std::unique_lock<std::mutex> lock(_mutex);

                    _wake_workers.wait(lock, [&] { return _stopping || !_queue.empty(); });

                    if (_queue.empty()) {
                        return;         // stopping, and nothing left worth finishing
                    }

                    index = _queue.front();
                    _queue.pop_front();

                    // moved out, not copied: nothing reads a slot's commands again once its worker has
                    // them, so this is also where the strings are released rather than being kept for
                    // the whole run alongside the outcome
                    command = std::move(_slots[index].command);
                    dump_commands = std::move(_slots[index].dump_commands);
                }

                CaseOutcome outcome = run_case(_cases[index], command, dump_commands);

                {
                    std::lock_guard<std::mutex> lock(_mutex);

                    _slots[index].outcome = std::move(outcome);
                    _slots[index].state = Slot::State::t_done;
                }

                _slot_done.notify_one();     // the main thread is the only waiter, ever
            }
        }

        const std::vector<DiscoveredCase> &_cases;
        const fs::path _root;
        std::vector<Slot> _slots;
        std::deque<size_t> _queue;
        std::vector<std::thread> _workers;
        bool _stopping = false;

        std::mutex _mutex;
        std::condition_variable _wake_workers;
        std::condition_variable _slot_done;
    };

    // a `.eco` that belongs to a *module* rather than being a case of its own: it lives at or below a
    // directory holding a `module.eco`, so a manifest is what claims it.
    //
    // the corpus rule is that an unpaired `.eco` is an error, never a silent skip - a program nothing
    // asserts is invisible. A library fixture's sources are the one legitimate exception.
    //
    // **the exception is the manifest's *presence*, not its contents.** This tests for the filename
    // `module.eco`, the same convention Parser::read_module_manifest resolves a `depends` directory
    // through; it does not read the manifest, so a source under a module directory that the manifest's
    // `#[sources:]` does not actually name is skipped here too. Reading it would close that hole, and
    // would need a policy for the fixtures whose manifests are deliberately unparseable
    // (`modules/bad_attribute`, `modules/empty_sources`) - which is why it is not done yet
    bool belongs_to_a_module(const fs::path &eco, const fs::path &root)
    {
        // walks down from the root rather than up from the file: `eco` always sits under `root` (the
        // caller got it from a recursive iterator over it), so descending needs one termination
        // condition where ascending needed a guard against walking off the top
        fs::path directory = root;

        for (const auto &part : fs::relative(eco.parent_path(), root)) {
            if (fs::exists(directory / "module.eco")) {
                return true;
            }

            directory /= part;
        }

        return fs::exists(directory / "module.eco");
    }

    Corpus discover_corpus(const fs::path &root)
    {
        Corpus corpus;
        std::vector<fs::path> eco_files;
        std::vector<fs::path> test_files;

        for (auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            if (entry.path().extension() == ".eco" && !belongs_to_a_module(entry.path(), root)) {
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
            discovered.rel = fs::relative(eco, root).generic_string();
            discovered.has_test_file = fs::exists(test_path);

            if (discovered.has_test_file
                && !EchoTests::parse_eco_test_file(test_path, discovered.test, discovered.parse_error)) {
                discovered.test.checks.clear();
            }

            if (discovered.runnable()) {
                discovered.scratch = case_scratch_dir(eco, root);

                if (discovered.test.mode == EchoTests::RunMode::t_build) {
                    discovered.binary = discovered.scratch / eco.stem();
                }
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

    // the pool, cached for the same reason and with the same lifetime as the corpus it runs
    ResultCache &results()
    {
        static ResultCache cache(corpus().cases, ECO_E2E_TESTS_DIR);

        return cache;
    }
};

TEST_CASE("eco end-to-end", "[e2e]")
{
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

    for (size_t index = 0; index < discovered.cases.size(); index++) {
        const DiscoveredCase &entry = discovered.cases[index];

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

            const CaseOutcome &outcome = results().at(index);

            if (entry.test.mode == EchoTests::RunMode::t_build) {
                check_build_output(entry.test, entry.binary, outcome);
            }
            else {
                check_program_output(entry.test, outcome.primary, "echoc");
            }
        }

        // the optional other half: what the emitted IR or either AST dump must contain. empty for a
        // case that did not parse, so a broken `.test` reports its parse error once and not per section
        for (size_t check = 0; check < entry.test.checks.size(); check++) {
            const EchoTests::CheckSection &section = entry.test.checks[check];

            DYNAMIC_SECTION(EchoTests::dump_section_name(section.kind) << ": " << entry.rel)
            {
                INFO("file: " << entry.eco.string());
                check_dump_section(entry.test, section, results().at(index).dumps.at(check));
            }
        }
    }
}
