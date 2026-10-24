#include "Compiler/TestRunner.h"

#include "Compiler/HostTool.h"
#include "Compiler/ProgressReporter.h"

#include <chrono>

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
    unsigned timeout_ms
)
{
    TestResult result;
    result.test = test;

    const auto started = std::chrono::steady_clock::now();

    const CapturedProcess captured = run_captured(
        argv,
        timeout_ms,
        {},
        { { k_isolated_test_env, test.symbol } });

    result.milliseconds = progress_elapsed_ms(started);
    result.output = captured.output;
    result.status = captured.exit_code;
    result.signal = captured.signal;

    result.outcome = isolated_outcome(test, captured.timed_out, captured.signal, result.status);
    return result;
}
