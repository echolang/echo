#ifndef TESTRUNNER_H
#define TESTRUNNER_H

#pragma once

#include "Compiler/TestSelection.h"

#include <functional>
#include <string>

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

    // **is running a test in a child process possible on this platform at all.**
    //
    // false is not a diagnostic - the caller writes that - but it exists so the refusal happens at the
    // subcommand rather than at the first test, which is the same call `#[link: framework]` makes about
    // Darwin: a platform that cannot do the thing is told so where the thing was asked for
    bool test_isolation_available();

    // **runs one test in a forked child and reports how it ended.**
    //
    // fork rather than anything in-process, and that is the design: a failed `assert` lowers to
    // `__eco_abort`, which writes its message and calls `exit(1)`. There is no unwind, no landing pad and no
    // signal handler anywhere in this compiler or in the standard library, so "continue after a failure" is
    // either a new kind of assertion that skips every destructor between itself and the runner, or a process
    // boundary. A boundary survives `assert`, `die`, `env::exit` and a segfault alike, and changes nothing
    // about how any of them lower.
    //
    // `call` is the test's body, reached through its address in the JIT - the child's whole job. It runs
    // **only in the child**, which is what makes the parent's own state irrelevant to it.
    //
    // the parent flushes its streams before forking, or whatever it had buffered is duplicated into every
    // child and written again by each
    // `timeout_ms` is `--timeout`. zero waits forever. the parent watches the clock with
    // poll and SIGKILL, so the flag's unit is honest and a child that ignores SIGALRM
    // still dies. a fired deadline is `t_timed_out`, not `t_signalled`
    TestResult run_test_isolated(
        const TestCase &test,
        const std::function<void()> &call,
        unsigned timeout_ms = 0);
};

#endif
