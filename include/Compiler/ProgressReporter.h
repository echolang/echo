#ifndef PROGRESSREPORTER_H
#define PROGRESSREPORTER_H

#pragma once

#include "Compiler/TerminalCapabilities.h"

#include <chrono>
#include <filesystem>
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
        t_emit
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

        // advanced one frame per redraw, and a redraw only happens when something changed - so the motion
        // is honest rather than a clock. See the class comment for why there is no thread
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
    // **there is no spinner thread**, for three independent reasons. Compiler::run_tool hands fd 2 to a
    // child and blocks while it writes, and there is no lock either side can take because the child is
    // another process. MCJIT runs the user's program on this thread. And motion carrying no information
    // is decoration. So the frame advances once per redraw and a redraw happens only when this object is
    // told something changed - which also makes the frame index a function of the tick sequence, and the
    // unit test byte-comparable.
    //
    // **process-wide, for Compiler::PhaseTimings' reason.** The rows are driven from the driver, which
    // could hold a reference - but the *obligation* is not the driver's: Compiler::run_tool hands its
    // stderr to a child and AST::DiagnosticRenderer writes to the same stream from inside the AST layer.
    // Threading a UI reference into HostTool and into the renderer to discharge it would put this
    // question in three objects; a singleton puts it in one and costs those two call sites a line each.
    //
    // **it owns no clock.** `commit` and `close` take milliseconds as a parameter and Compiler::ProgressStep
    // holds the steady_clock, which is what lets a test hand this a literal 182 and compare bytes - and
    // what keeps the drawing code from being a second reading of a time PhaseTimings already measures
    class ProgressReporter
    {
    public:

        // disabled: every entry point below is a no-op. The default state, so a caller that never enables
        // one is not a caller that has to branch - the shape PhaseTimings::_enabled already takes
        ProgressReporter() = default;

        // the testable constructor. An ostringstream and a forced TerminalCapabilities is the whole of
        // what a byte-for-byte assertion needs, which is why there is no --progress=always
        ProgressReporter(std::ostream &out, TerminalCapabilities capabilities);

        static ProgressReporter &instance();

        void enable(std::ostream &out, TerminalCapabilities capabilities);

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
        // and a forgotten one costs one garbled line rather than a lost row
        void suspend();

        // the closing line, and the end of the checklist. After this the stream belongs to whatever comes
        // next - under `run` that is the program itself
        void close(const std::string &what, unsigned int milliseconds);

    private:

        std::ostream *_out = nullptr;
        TerminalCapabilities _capabilities;
        ProgressTheme _theme = ProgressTheme::ascii();

        // what the live row says, kept so suspend() can be undone by the next write with no caller
        // co-operation. No phase means nothing is live
        std::optional<ProgressPhase> _live_phase;
        std::string _live_subject;
        std::string _live_detail;
        size_t _frame = 0;

        // whether the live row is currently on screen. Distinct from `_live_phase`: between a suspend()
        // and the next write there is a row to redraw and nothing drawn
        bool _drawn = false;

        // one row, already truncated to fit. The single place a column width is spelled
        std::string render_row(
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

        void draw_live_row();
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
