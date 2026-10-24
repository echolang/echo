#ifndef TESTREPORTER_H
#define TESTREPORTER_H

#pragma once

#include "Compiler/ProgressReporter.h"
#include "Compiler/TerminalCapabilities.h"
#include "Compiler/TestRunner.h"

#include <chrono>
#include <ostream>
#include <vector>

namespace Compiler
{
    // **how much of a run is reported, which is not the same question as through what.**
    //
    // the medium is the terminal's answer and is `TestReporter::is_live()`; this is the invocation's, and
    // `--verbose` is the only thing that sets it. Keeping them apart is what makes a listing read the same
    // on a terminal as in a CI log: a detail level that also switched medium would be two changes at once,
    // and the one a reader compares against is the log
    enum class TestDetail
    {
        // the default. a live counter row where there is a terminal, one line per test where there is not,
        // and a failure's output under the summary either way
        t_counter,

        // every test under the file it is written in, with what it cost and whatever it printed. Written
        // to the results stream whether or not a checklist could have been drawn, so the transcript a
        // person reads is the transcript a pipe stores
        t_listing
    };

    // what a test run says. terminal: a live counter, a row per failure, a summary, then captured
    // output. otherwise: one plain line per test, then the same summary. ProgressReporter writes
    // nothing off a terminal, so results cannot live only there. results to stdout, refusals to stderr
    class TestReporter
    {
    public:
        // `out` is where results go - stdout in practice. The capabilities are stderr's, because they are
        // what decided whether a checklist is being drawn at all, and this has to make the same choice or
        // the two channels both write.
        //
        // `detail` is the other axis, and the two are settled independently - see Compiler::TestDetail
        TestReporter(
            std::ostream &out,
            ProgressReporter &progress,
            TerminalCapabilities capabilities,
            TestDetail detail = TestDetail::t_counter);

        // how many tests are about to run. Opens the live row, or prints nothing at all
        void begin(size_t total);

        // one finished test, reported as it finishes. Failures are *counted and kept* here rather than
        // printed in full: their output goes under the summary, where a reader is not reading it interleaved
        // with the next test's progress
        void result(const TestResult &result);

        // the summary and then every failure in full. Returns true when nothing failed, which is what the
        // subcommand's exit status is
        bool finish();

    private:
        // is the live checklist being drawn - the one question that decides which of the two renderings this
        // is. Asked of the progress reporter rather than re-derived from the capabilities, so the two cannot
        // disagree about whether a row is on screen.
        //
        // **a listing is never live.** It writes committed lines and file headers, and a mutable row below
        // them would be a second writer on a stream this object has already started using
        bool is_live() const;

        // one test as a listing's line: the mark, the name, what it cost, and why it failed where it did
        void write_listing_line(const TestResult &result);

        // the count and the total under the file whose tests just ended, and nothing at all before the
        // first one. **After its tests rather than on its header**, because the header is written when the
        // first result of a file arrives and the total is not known until the last one has
        void close_listing_file();

        std::ostream &_out;
        ProgressReporter &_progress;
        TerminalCapabilities _capabilities;
        TestDetail _detail;

        // the marks, off Compiler::ProgressTheme rather than spelled here: a failure's glyph is the
        // checklist's answer, and this renderer is the second channel reporting the same failure
        ProgressTheme _theme;

        size_t _total = 0;
        size_t _done = 0;
        std::vector<TestResult> _failed;

        // the listing's grouping, and it is a comparison rather than a plan: the tests of one file arrive
        // together because that is the order they were selected in, and a file that came back a second time
        // would get a second header rather than a wrong subtotal
        std::string _file;
        size_t _file_tests = 0;
        unsigned int _file_milliseconds = 0;

        // **the clock is this object's**, which is the role Compiler::ProgressStep plays for every other
        // phase: the reporter owns no clock at all and takes milliseconds as a parameter. Wall time from the
        // first fork to the last reap rather than the sum of the tests' own, because forking a few hundred
        // processes is time the run took and a number that omitted it would not add up
        std::chrono::steady_clock::time_point _started;
    };
};

#endif
