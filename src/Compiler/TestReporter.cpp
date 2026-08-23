#include "Compiler/TestReporter.h"

#include "Compiler/TerminalStyle.h"

#include <fmt/core.h>

#include <cstring>
#include <sstream>

namespace
{
    // what ended a test, said the way a person would. `strsignal` rather than a table of our own: the names
    // are the platform's, and a signal this compiler has never heard of still has one. Windows has no
    // `strsignal` and no POSIX signals; the number is still the honest report
    std::string signal_description(int signal)
    {
#if defined(_WIN32)
        return fmt::format("signal {}", signal);
#else
        const char *described = strsignal(signal);

        if (described == nullptr) {
            return fmt::format("signal {}", signal);
        }

        return fmt::format("{} (signal {})", described, signal);
#endif
    }

    // the one sentence that says how a test ended, shared by both renderings so a CI log and a terminal
    // never disagree about what happened
    std::string outcome_sentence(const Compiler::TestResult &result)
    {
        switch (result.outcome) {
        case Compiler::TestOutcome::t_passed:
            return "passed";

        case Compiler::TestOutcome::t_signalled:
            return fmt::format("killed by {}", signal_description(result.signal));

        case Compiler::TestOutcome::t_failed:
            if (result.test.expects_death && result.status == 0) {
                return "expected death, but it returned";
            }

            return fmt::format("exited {}", result.status);

        case Compiler::TestOutcome::t_timed_out:
            return "timed out";
        }

        return "ended";
    }

    // how wide a listing's name column is. A name longer than this pushes the milliseconds right rather
    // than being cut: a listing is a transcript, and a truncated name is one a `--filter` cannot be typed
    // from
    constexpr size_t LISTING_NAME_WIDTH = 46;

    // the same column width Compiler::ProgressReporter gives a row's elapsed field, so a listing's times
    // line up under a checklist's
    constexpr size_t LISTING_MILLISECONDS_WIDTH = 6;

    // `1 test` / `4 tests`, which three lines here need and none of them should spell
    std::string test_count(size_t count)
    {
        return fmt::format("{} test{}", count, count == 1 ? "" : "s");
    }

    // every line of a failing test's output, indented under it. **indented rather than passed through**,
    // because a test's output is being quoted here rather than written: without the indent an `assert`'s two
    // lines read as though echoc had printed them
    void write_indented(std::ostream &out, const std::string &text, const char *indent)
    {
        std::istringstream lines(text);
        std::string line;

        while (std::getline(lines, line)) {
            out << indent << line << "\n";
        }
    }
};

Compiler::TestReporter::TestReporter(
    std::ostream &out,
    ProgressReporter &progress,
    TerminalCapabilities capabilities,
    TestDetail detail
) :
    _out(out), _progress(progress), _capabilities(capabilities), _detail(detail),
    // the same theme the checklist draws with, derived once here for the same reason it is derived once
    // there: a failure's mark is one fact, and two sites asking `unicode` are two chances to answer it
    // differently
    _theme(capabilities.unicode ? ProgressTheme::pretty() : ProgressTheme::ascii())
{
}

bool Compiler::TestReporter::is_live() const
{
    return _progress.enabled() && _detail == TestDetail::t_counter;
}

void Compiler::TestReporter::begin(size_t total)
{
    _total = total;
    _started = std::chrono::steady_clock::now();

    if (!is_live()) {
        // **a listing owes the checklist a suspend and nothing else.** Sticky, unpaired, and it is what
        // makes a listing's first header start on a line of its own however the compile above it ended.
        // A pipe reaches here too, where every entry point on that object is already a no-op
        if (_detail == TestDetail::t_listing) {
            _progress.suspend();
        }

        return;
    }

    // one row for the whole run, and it is opened once. `open` commits whatever was live as *failed*, so a
    // row per test would be a row per test that reads as a failure the moment the next one starts
    _progress.open(ProgressPhase::t_test, test_count(total));
    _progress.tick(fmt::format("0/{}", total));
}

void Compiler::TestReporter::result(const TestResult &result)
{
    _done += 1;

    if (result.failed()) {
        _failed.push_back(result);
    }

    if (_detail == TestDetail::t_listing) {
        const std::string file = test_display_file(result.test);

        // a header per file, decided by the file changing rather than by counting: the tests of one arrive
        // together, and nothing here has to be told how many there were going to be
        if (file != _file) {
            close_listing_file();

            _out << file << "\n";
            _file = file;
        }

        _file_tests += 1;
        _file_milliseconds += result.milliseconds;

        write_listing_line(result);
        return;
    }

    if (!is_live()) {
        // the plain rendering, and the shape is deliberate: the outcome first, so a reader scanning a log
        // finds the failures down one column, and the counter beside it so a truncated log still says how
        // far it got
        const char *mark = result.failed() ? "FAIL" : "ok";

        _out << fmt::format("{:<4} {}/{}  {}", mark, _done, _total, test_display_name(result.test));

        if (result.failed()) {
            _out << fmt::format("  ({})", outcome_sentence(result));
        }

        _out << "\n";
        _out.flush();

        return;
    }

    // a failure gets a committed row of its own, *above* the counter still running - so the run reads as it
    // happens and a reader does not have to wait for the summary to know something went wrong. A pass gets
    // no row: 400 committed lines saying `✓` is not information
    if (result.failed()) {
        _progress.row(
            ProgressPhase::t_test,
            result.test.name,
            outcome_sentence(result),
            ProgressState::t_failed);
    }

    _progress.tick(fmt::format("{}/{} {}", _done, _total, result.test.name));
}

void Compiler::TestReporter::write_listing_line(const TestResult &result)
{
    const bool failed = result.failed();

    // the mark is the only coloured thing on the line, which is Compiler::ProgressReporter's rule for a row
    // and it is the same reason: an SGR sequence has no width, so colouring anything that is measured is how
    // a column stops lining up
    _out << "  "
         << styled(failed ? _theme.failed : _theme.done,
                   failed ? sgr::error : sgr::success,
                   _capabilities.color);

    _out << fmt::format(
        " {:<{}} {:>{}} ms",
        result.test.name,
        LISTING_NAME_WIDTH,
        result.milliseconds,
        LISTING_MILLISECONDS_WIDTH
    );

    if (!result.test.group.empty()) {
        _out << fmt::format("  ({})", result.test.group);
    }

    if (failed) {
        _out << fmt::format("  {}", outcome_sentence(result));
    }

    _out << "\n";

    // **a passing test's output too, and that is the whole of what this rendering buys.** It is captured
    // for every test and quoted for none that passed, so a `dprint` in a green test has no other way out
    if (!result.output.empty()) {
        write_indented(_out, result.output, "      ");
    }

    _out.flush();
}

void Compiler::TestReporter::close_listing_file()
{
    if (_file.empty()) {
        return;
    }

    _out << fmt::format("  {}, {} ms\n\n", test_count(_file_tests), _file_milliseconds);

    _file.clear();
    _file_tests = 0;
    _file_milliseconds = 0;
}

bool Compiler::TestReporter::finish()
{
    const bool ok = _failed.empty();

    const unsigned int elapsed = progress_elapsed_ms(_started);

    const ProgressState state = ok ? ProgressState::t_done : ProgressState::t_failed;

    const std::string summary = ok
        ? fmt::format("{} passed", test_count(_total))
        : fmt::format("{}, {} failed", test_count(_total), _failed.size());

    // the last file's own line, before anything about the run as a whole
    close_listing_file();

    if (is_live()) {
        // **the counter row is committed here and not left to close().** A row still live when the checklist
        // closes is committed as *failed*, which is right for a step that never said it finished and wrong
        // for this one - it is the same self-report every ProgressStep makes, made by hand because a test
        // run's outcome is not "did the phase run"
        _progress.set_detail(fmt::format("{}/{}", _done, _total));
        _progress.commit(state, elapsed);

        _progress.close(summary, elapsed, state);
    }
    else {
        _out << summary << "\n";
    }

    // **the captured output comes after the summary and never during the run.** During it, the terminal has
    // one mutable line and writing several into that region is exactly what ProgressReporter exists to stop;
    // in a log, a failure's output interleaved with the next test's line reads as the next test's. So both
    // renderings quote it here, in full, in the order the tests ran
    for (const TestResult &failure : _failed) {
        _out << "\n"
             << styled(
                    fmt::format("{} {}", _theme.failed, test_display_name(failure.test)),
                    sgr::error, _capabilities.color);

        if (!failure.test.group.empty()) {
            _out << fmt::format("  (group: {})", failure.test.group);
        }

        _out << fmt::format("  {}\n", outcome_sentence(failure));

        if (!failure.output.empty()) {
            write_indented(_out, failure.output, "    ");
        }
    }

    _out.flush();

    return ok;
}
