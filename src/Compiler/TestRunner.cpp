#include "Compiler/TestRunner.h"

#include "Compiler/HostTool.h"
#include "Compiler/ProgressReporter.h"

#include <chrono>
#include <string_view>

namespace
{
    // one pass/fail rule for the linked runner, so a Windows spawn and a Unix one cannot disagree.
    // a fired `--timeout` stays t_timed_out. a signal stays t_signalled: a segfault is not
    // `__eco_abort`, and v1 does not count it as an expected death. a non-zero *exit* is the
    // inverted pass when the test asked for death - die and a failed assert are both that exit
    Compiler::TestOutcome isolated_outcome(
        const Compiler::TestCase &test,
        bool timed_out,
        int signal,
        int status
    )
    {
        if (timed_out) {
            return Compiler::TestOutcome::t_timed_out;
        }

        if (signal != 0) {
            return Compiler::TestOutcome::t_signalled;
        }

        if (test.expects_death) {
            return status != 0
                ? Compiler::TestOutcome::t_passed
                : Compiler::TestOutcome::t_failed;
        }

        return status == 0
            ? Compiler::TestOutcome::t_passed
            : Compiler::TestOutcome::t_failed;
    }
};

Compiler::TestResult Compiler::run_test_isolated(
    const TestCase &test,
    const std::vector<std::string> &argv,
    unsigned timeout_ms,
    const std::vector<std::pair<std::string, std::string>> &extra_env
)
{
    TestResult result;
    result.test = test;

    const auto started = std::chrono::steady_clock::now();

    // the closed identity a test library reads to say which test it is in. set on every run, not
    // only under --coverage: events, snapshots and a coverage file all join on ECHO_TEST_ID
    std::vector<std::pair<std::string, std::string>> env = extra_env;
    env.emplace_back(k_isolated_test_env, test.symbol);
    env.emplace_back("ECHO_TEST_ID", test_display_name(test));
    env.emplace_back("ECHO_TEST_NAME", test.name);
    env.emplace_back("ECHO_TEST_FILE", test.file.string());
    env.emplace_back("ECHO_TEST_MODULE", test.module);
    env.emplace_back("ECHO_TEST_GROUP", test.group);

    const CapturedProcess captured = run_captured(
        argv,
        timeout_ms,
        {},
        env);

    result.milliseconds = progress_elapsed_ms(started);
    result.stdout_text = captured.stdout_text;
    result.stderr_text = captured.stderr_text;
    result.output = captured.output;
    result.status = captured.exit_code;
    result.signal = captured.signal;

    const std::string_view abort_text = !captured.stderr_text.empty()
        ? std::string_view(captured.stderr_text)
        : std::string_view(captured.output);
    result.failure = parse_abort_report(abort_text);

    result.outcome = isolated_outcome(test, captured.timed_out, captured.signal, result.status);
    return result;
}
