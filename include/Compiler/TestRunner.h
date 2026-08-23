#ifndef TESTRUNNER_H
#define TESTRUNNER_H

#pragma once

#include "Compiler/TestSelection.h"

#if defined(__unix__) || defined(__APPLE__)
#include <functional>
#endif
#include <string>
#include <vector>

namespace Compiler
{
    // how a test ended. **three answers rather than a bool**, because the two ways of failing are not the
    // same news: a non-zero exit is the test saying so - a failed `assert`, a `die` - while a signal is the
    // test being taken down, and only one of those has a message worth reading above it
    enum class TestOutcome
    {
        t_passed,
        t_failed,
        t_signalled,

        // the deadline `--timeout` asked for fired. a fourth answer rather than folding it into
        // t_signalled: a test killed by SIGALRM because it ran too long and a test killed by
        // SIGSEGV are not the same news
        t_timed_out
    };

    // one finished test.
    //
    // `output` is everything the test wrote on either stream, captured rather than inherited: a passing
    // test's chatter has no place in a summary, and a failing one's is the whole of what a reader wants.
    // That is the opposite call Compiler::run_tool makes, and for the opposite reason - a clang diagnostic
    // should reach the terminal as clang wrote it, where a test's output belongs *under* its own row
    struct TestResult
    {
        TestCase test;
        TestOutcome outcome = TestOutcome::t_passed;

        // the signal that ended it, 0 unless `outcome` is t_signalled
        int signal = 0;

        // the exit status, when it exited at all
        int status = 0;

        unsigned int milliseconds = 0;

        std::string output;

        bool failed() const {
            return outcome != TestOutcome::t_passed;
        }
    };

    // the environment variable a linked test runner looks up to decide which test to call.
    // the parent sets it per child so a parallel spawn cannot leak the name into this process
    constexpr const char *k_isolated_test_env = "ECO_INTERNAL_RUN_TEST";

    // **runs one test in a child process and reports how it ended.**
    //
    // a process boundary rather than anything in-process, and that is the design: a failed `assert`
    // lowers to `__eco_abort`, which writes its message and calls `exit(1)`. There is no unwind, no
    // landing pad and no signal handler anywhere in this compiler or in the standard library, so
    // "continue after a failure" is either a new kind of assertion that skips every destructor
    // between itself and the runner, or a process boundary. A boundary survives `assert`, `die`,
    // `env::exit` and a segfault alike, and changes nothing about how any of them lower.
    //
    // `#[tests: expects death]` inverts the pass condition: a non-zero *exit* is a pass. a fired
    // `--timeout` stays `t_timed_out`. a signal stays `t_signalled`. v1 does not distinguish `die`
    // from a failed `assert` - both are `__eco_abort` - and does not match the abort message.
    //
    // two ways to start the child, one result shape. the JIT path forks and calls `call` in the
    // child. a linked runner is spawned as `argv` with `ECO_INTERNAL_RUN_TEST` naming this test.
    // `timeout_ms` is `--timeout`. zero waits forever. a fired deadline is `t_timed_out`

#if defined(__unix__) || defined(__APPLE__)
    TestResult run_test_isolated(
        const TestCase &test,
        const std::function<void()> &call,
        unsigned timeout_ms = 0);
#endif

    TestResult run_test_isolated(
        const TestCase &test,
        const std::vector<std::string> &argv,
        unsigned timeout_ms = 0);
};

#endif
