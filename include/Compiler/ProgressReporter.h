#ifndef PROGRESSREPORTER_H
#define PROGRESSREPORTER_H

#pragma once

#include "Compiler/TerminalCapabilities.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace Compiler
{
    // the pipeline as a *user waiting on it* sees it, which is deliberately coarser than the ~15 phases
    // Compiler::PhaseTimings breaks a compile into.
    //
    // a closed set rather than free strings, for two reasons that agree: the label column is a fixed
    // width and a streaming checklist cannot measure it afterwards the way PhaseTimings::report does, and
    // a phase added here must answer `progress_phase_label` or fail to compile.
    //
    // **the spellings are PhaseTimings' spellings** wherever the two describe the same thing. One
    // vocabulary for one pipeline: a reader who saw `emit + link` under `-t` must not have to learn that
    // the checklist calls it something else. `t_cached` is the one label with no phase behind it - it
    // reports work that did *not* happen, which is the only interesting thing a cache has to say
    enum class ProgressPhase
    {
        t_parse,
        t_semantic_passes,
        t_cached,
        t_cc,
        t_codegen,
        t_optimize,
        t_emit,

        // running a test, which is the one phase that is not a step of a compile - what `echoc test` does
        // *after* every other row has been committed. It shares this vocabulary rather than having one of
        // its own because it shares the line: Compiler::ProgressReporter is the only thing allowed to move
        // this cursor, so a test run reports through it or beside it, and beside it is two writers again
        t_test
    };

    // a switch with no default, so a phase added above does not compile until it has a word
    const char *progress_phase_label(ProgressPhase phase);

    // what a row says happened. `t_skipped` is not a weaker `t_done`: it is work that did not run
    enum class ProgressState
    {
        t_running,
        t_done,
        t_failed,
        t_skipped
    };

    // the glyphs a checklist row is drawn with, and nothing else. Shaped like AST::DiagnosticTheme and
    // deliberately **not** that struct: its six fields are named for the parts of a diagnostic frame -
    // gutter, underline_rule, primary_mark - and a spinner beside them would be a field the renderer
    // reads never and this reads always, which is one struct answering two questions.
    //
    // `failed` is the same `x` DiagnosticTheme::ascii() draws, because a failure in the two channels is
    // one fact and must not look like two
    struct ProgressTheme
    {
        const char *done;
        const char *failed;
        const char *skipped;

        // advanced one frame per clock tick (~80 ms) when the interactive checklist is animating, and
        // one frame per `tick()` when it is not. Pretty frames are the same byte width as each other;
        // ascii frames are too - that is what lets the clock overwrite the glyph without rebuilding
        std::vector<const char *> spinner;

        static ProgressTheme pretty();
        static ProgressTheme ascii();
    };

    // **the sole answer to "what is the compiler doing right now, and is a partial line on the terminal
    // because of it".**
    //
    // one mutable trailing line; everything above it is committed and is never rewritten. The whole
    // vocabulary is a carriage return and an erase-to-end-of-line - no cursor-up, no alternate screen -
    // because cursor-up addresses a line by *count*, and a foreign newline from a clang subprocess
    // changes the count with nothing to detect it. Erase-to-end-of-line is wrong only about the line the
    // cursor is already on.
    //
    // two invariants hold the model up:
    //
    //   1. it always leaves the cursor at column 0 of a line it is free to overwrite. That is what makes
    //      a foreign writer composable - whoever writes next starts on a blank line, exactly as if
    //      nothing had drawn.
    //   2. a rendered row never exceeds `width - 1` columns. The erase clears to the end of the
    //      *physical* line, so a row that wrapped occupies two and the carriage return lands at the start
    //      of the second - leaving the first half of the previous frame on screen permanently. `width` of
    //      0 falls back to 80 here rather than meaning "do not wrap": a line that cannot be measured must
    //      still be short.
    //
    // **the spinner is a thread. the compile is not.** `enable(..., true)` starts a clock that overwrites
    // the glyph and elapsed field of the row already on screen. The constructor the unit tests use does
    // not animate. Join before `fork` and before any other writer takes this stream: `suspend()` joins
    // then erases, `close()` joins, `ClockPause` is what a `fork` that does not go through `suspend`
    // owes. The rest is notes/progress.md.
    //
    // **process-wide, for Compiler::PhaseTimings' reason.** The rows are driven from the driver, which
    // could hold a reference - but the *obligation* is not the driver's: Compiler::run_tool hands its
    // stderr to a child and AST::DiagnosticRenderer writes to the same stream from inside the AST layer.
    // Threading a UI reference into HostTool and into the renderer to discharge it would put this
    // question in three objects; a singleton puts it in one and costs those two call sites a line each.
    //
    // **`commit` and `close` still take milliseconds as a parameter.** Compiler::ProgressStep holds the
    // steady_clock that becomes the committed number, which is what lets a test hand this a literal 182
    // and compare bytes. The live field the clock paints is a different paint of the same start: `open`
    // records it so the thread can write digits without asking ProgressStep, which lives on the driver.
    class ProgressReporter
    {
    public:

        // disabled: every entry point below is a no-op. The default state, so a caller that never enables
        // one is not a caller that has to branch - the shape PhaseTimings::_enabled already takes
        ProgressReporter();

        // the testable constructor. An ostringstream and a forced TerminalCapabilities is the whole of
        // what a byte-for-byte assertion needs, which is why there is no --progress=always. Does not
        // animate: the frame index stays a function of the tick sequence
        ProgressReporter(std::ostream &out, TerminalCapabilities capabilities);

        ~ProgressReporter();

        ProgressReporter(const ProgressReporter &) = delete;
        ProgressReporter &operator=(const ProgressReporter &) = delete;
        ProgressReporter(ProgressReporter &&) = delete;
        ProgressReporter &operator=(ProgressReporter &&) = delete;

        static ProgressReporter &instance();

        // `animate` is what lets a live row start the clock thread that redraws the glyph and elapsed
        // field. `clock_fd` is the descriptor that thread `write`s; -1 means stderr. A test that wants
        // the clock without touching the process's stderr passes the write end of a pipe
        void enable(
            std::ostream &out,
            TerminalCapabilities capabilities,
            bool animate = false,
            int clock_fd = -1
        );

        bool enabled() const { return _out != nullptr; }

        // opens the one mutable trailing line. Whatever was live is committed as failed first, so a row
        // can never be silently lost by a caller that forgot to close it
        void open(ProgressPhase phase, const std::string &subject);

        // redraws the live row with new detail and advances the spinner one frame
        void tick(const std::string &detail);

        // the same field, changed without a redraw. What a step's summary takes: by the time it is known
        // the row is about to be committed, and drawing a frame nothing will read is a write whose only
        // effect is to make the committed bytes depend on how the row was filled in
        void set_detail(const std::string &detail);

        // clears the live row, writes the finished one with a newline, then the detail lines under it.
        // From here that line is committed and is never rewritten
        void commit(
            ProgressState state,
            unsigned int milliseconds,
            const std::vector<std::filesystem::path> &details = {});

        // for something already decided by the time it can be said - the reused modules are the case, and
        // they are decided by the cache plan long before codegen runs
        void row(
            ProgressPhase phase,
            const std::string &subject,
            const std::string &detail,
            ProgressState state);

        // erases the live row and leaves the cursor at column 0. **Sticky and idempotent**: there is no
        // resume() to forget, because the next open/tick/commit/row redraws from the state this object
        // still holds. So a caller about to write into this stream owes exactly one call and no pairing,
        // and a forgotten one costs one garbled line rather than a lost row.
        //
        // joins the clock first, then erases. thread-safe with itself: C compiles may each call this
        // from a worker whose driver already erased the row, and two no-ops racing on `_drawn` is a
        // data race
        void suspend();

        // joins the clock and leaves the row on screen. What a caller about to `fork` owes when it
        // does not go through `suspend()`: the child does not inherit a live thread. Restarts the
        // clock in the parent destructor. The child `_exit`s, so it does not run this
        class ClockPause
        {
        public:

            explicit ClockPause(ProgressReporter &reporter);
            ~ClockPause();

            ClockPause(const ClockPause &) = delete;
            ClockPause &operator=(const ClockPause &) = delete;
            ClockPause(ClockPause &&) = delete;
            ClockPause &operator=(ClockPause &&) = delete;

        private:

            ProgressReporter &_reporter;
        };

        // the closing line, and the end of the checklist. After this the stream belongs to whatever comes
        // next - under `run` that is the program itself.
        //
        // `state` is the outcome of what is being *closed*, and the only reason it is a parameter is that a
        // test run can end having done everything it was asked and still have failed. A compile that got
        // this far succeeded by definition, which is why it defaults
        void close(
            const std::string &what,
            unsigned int milliseconds,
            ProgressState state = ProgressState::t_done);

    private:

        std::ostream *_out = nullptr;
        TerminalCapabilities _capabilities;
        ProgressTheme _theme = ProgressTheme::ascii();

        // only `suspend()` takes this, because that is the one entry a worker calls. Drawing the
        // live row stays on the driver
        std::mutex _suspend;

        // what the live row says, kept so suspend() can be undone by the next write with no caller
        // co-operation. No phase means nothing is live
        std::optional<ProgressPhase> _live_phase;
        std::string _live_subject;
        std::string _live_detail;
        size_t _frame = 0;

        // whether the live row is currently on screen. Distinct from `_live_phase`: between a suspend()
        // and the next write there is a row to redraw and nothing drawn
        bool _drawn = false;

        bool _animate = false;
        std::chrono::steady_clock::time_point _live_started;

        // thread, cached line, glyph/ms offsets, fd. Incomplete here so this header does not pull
        // <thread> into every translation unit that only wants to tick a row
        struct Clock;
        std::unique_ptr<Clock> _clock;

        // glyph and elapsed offsets in `text`, so the clock overwrites slots `render_row` already
        // knows rather than recovering them with `find` and a tail length
        struct RenderedRow
        {
            std::string text;
            size_t glyph_offset = 0;
            size_t glyph_bytes = 0;
            size_t ms_offset = 0;
        };

        void halt_spinner();
        void continue_spinner();
        std::unique_lock<std::mutex> lock_clock();
        void write_live_row();

        // one row, already truncated to fit. The single place a column width is spelled
        RenderedRow render_row(
            ProgressPhase phase,
            const std::string &subject,
            const std::string &detail,
            const std::string &mark,
            const char *mark_sgr,
            std::optional<unsigned int> milliseconds) const;

        // a carriage return and an erase-to-end-of-line, or nothing when no row is drawn. **Deliberately
        // not routed through the colour gate**: these are cursor movement rather than SGR, so they are an
        // `interactive` question and `--color=never` on a terminal must not turn them off
        void erase_live_row();
    };

    // times a row for as long as it is in scope, so an early return cannot lose it. Deliberately the same
    // shape as Compiler::ScopedPhase, which is what it sits beside at every call site.
    //
    // **closes as failed if nobody said otherwise**, which is what covers the early returns and the
    // throws in main.cpp without any of them naming this object
    class ProgressStep
    {
    public:

        ProgressStep(ProgressReporter &reporter, ProgressPhase phase, const std::string &subject = "");

        ~ProgressStep();

        ProgressStep(const ProgressStep &) = delete;
        ProgressStep &operator=(const ProgressStep &) = delete;

        void tick(const std::string &detail);

        // the text in the row's own detail column - `21 files`, `2 sources`
        void summary(std::string text);

        // the lines committed under the row once it closes
        void detail(std::vector<std::filesystem::path> paths);

        // closes now rather than at scope exit, for the sites that learn the outcome before they report it
        void finish(bool ok);

    private:

        ProgressReporter &_reporter;
        std::chrono::steady_clock::time_point _start;
        std::vector<std::filesystem::path> _details;
        std::string _summary;
        bool _closed = false;
    };

    // milliseconds since `start`, rounded, which is what a row and the closing line both report. Whole
    // milliseconds and no total: precision and the breakdown are `--timings`' question and stay there
    unsigned int progress_elapsed_ms(std::chrono::steady_clock::time_point start);
};

#endif
