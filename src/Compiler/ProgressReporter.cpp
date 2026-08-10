#include "Compiler/ProgressReporter.h"

#include "Compiler/TerminalStyle.h"

#include <fmt/core.h>

#include <algorithm>

namespace
{
    // the columns a row is laid out in. `PHASE_WIDTH` is the length of the longest label the closed
    // vocabulary has - `semantic passes` - which is the second reason that vocabulary is an enum: a free
    // string could be longer than the column and there would be nowhere to notice
    constexpr size_t PHASE_WIDTH = 15;
    constexpr size_t SUBJECT_WIDTH = 12;
    constexpr size_t DETAIL_WIDTH = 12;
    constexpr size_t MILLISECONDS_WIDTH = 6;

    // how far the file lines under a committed row are indented - past the mark and into the phase column
    constexpr size_t DETAIL_LINE_INDENT = 7;

    // what a row is truncated to when the terminal did not answer its width. A row that cannot be
    // measured must still be short, which is the opposite of what a width of 0 means to a wrapper
    constexpr unsigned int ASSUMED_WIDTH = 80;

    // a carriage return, then erase-to-end-of-line. **Not gated on colour**: this is cursor movement
    // rather than SGR, and `--color=never` on a terminal must not turn a redraw into a stream of
    // half-overwritten rows
    constexpr const char *ERASE_ROW = "\r\x1b[K";

    // how many columns a string occupies, counting a UTF-8 sequence once. Every glyph either theme draws
    // is single-column, so leading bytes are the whole of the arithmetic - and doing it in bytes instead
    // is what would put the milliseconds of a unicode row two columns left of an ascii one
    size_t display_width(const std::string &text)
    {
        size_t width = 0;
        for (const char c : text) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                width++;
            }
        }

        return width;
    }

    std::string pad_to(const std::string &text, size_t columns)
    {
        const size_t width = display_width(text);
        if (width >= columns) {
            return text;
        }

        return text + std::string(columns - width, ' ');
    }

    // the *tail* of `text`, because the informative half of a path is the end of it. Returns the text
    // untouched when it already fits, and an empty string when there is not even room for the ellipsis
    std::string truncate_from_the_left(const std::string &text, size_t columns, const char *ellipsis)
    {
        if (display_width(text) <= columns) {
            return text;
        }

        const size_t ellipsis_width = display_width(ellipsis);
        if (columns <= ellipsis_width) {
            return "";
        }

        const size_t keep = columns - ellipsis_width;

        // walk back from the end over leading bytes, so the cut never lands inside a UTF-8 sequence
        size_t offset = text.size();
        size_t kept = 0;
        while (offset > 0 && kept < keep) {
            offset--;
            while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) {
                offset--;
            }
            kept++;
        }

        return std::string(ellipsis) + text.substr(offset);
    }

    std::string right_trimmed(const std::string &text)
    {
        const size_t end = text.find_last_not_of(' ');
        if (end == std::string::npos) {
            return "";
        }

        return text.substr(0, end + 1);
    }
};

const char *Compiler::progress_phase_label(ProgressPhase phase)
{
    switch (phase) {
    case ProgressPhase::t_parse:
        return "parse";
    case ProgressPhase::t_semantic_passes:
        return "semantic passes";
    case ProgressPhase::t_cached:
        return "cached";
    case ProgressPhase::t_cc:
        return "cc";
    case ProgressPhase::t_codegen:
        return "codegen";
    case ProgressPhase::t_optimize:
        return "optimize";
    case ProgressPhase::t_emit:
        return "emit + link";
    }

    return "";
}

Compiler::ProgressTheme Compiler::ProgressTheme::pretty()
{
    return ProgressTheme { "✓", "✗", "·", { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" } };
}

Compiler::ProgressTheme Compiler::ProgressTheme::ascii()
{
    return ProgressTheme { "+", "x", "-", { "|", "/", "-", "\\" } };
}

Compiler::ProgressReporter::ProgressReporter(std::ostream &out, TerminalCapabilities capabilities)
{
    enable(out, capabilities);
}

Compiler::ProgressReporter &Compiler::ProgressReporter::instance()
{
    static ProgressReporter reporter;
    return reporter;
}

void Compiler::ProgressReporter::enable(std::ostream &out, TerminalCapabilities capabilities)
{
    _out = &out;
    _capabilities = capabilities;

    // the theme is derived once, here, rather than per row - the rule AST::DiagnosticRenderer already
    // follows, and for its reason: three sites re-asking `unicode` are three chances to answer it
    // differently
    _theme = capabilities.unicode ? ProgressTheme::pretty() : ProgressTheme::ascii();
}

std::string Compiler::ProgressReporter::render_row(
    ProgressPhase phase,
    const std::string &subject,
    const std::string &detail,
    const std::string &mark,
    const char *mark_sgr,
    std::optional<unsigned int> milliseconds
) const
{
    const char *ellipsis = _capabilities.unicode ? "…" : "...";

    const std::string elapsed = milliseconds.has_value()
        ? fmt::format("{:>{}} ms", milliseconds.value(), MILLISECONDS_WIDTH)
        : std::string();

    // everything except the detail field, measured before it is filled - so the budget below is what is
    // actually left rather than what was hoped for
    const std::string head
        = "  " + mark + "  " + pad_to(progress_phase_label(phase), PHASE_WIDTH) + "  "
        + pad_to(subject, SUBJECT_WIDTH) + "  ";

    const unsigned int width = _capabilities.width > 0 ? _capabilities.width : ASSUMED_WIDTH;

    // one column short of the terminal, because the erase clears to the end of the *physical* line: a row
    // that wrapped sits on two of them and the carriage return lands at the start of the second
    const size_t limit = width > 1 ? width - 1 : 1;
    const size_t fixed = display_width(head) + display_width(elapsed);
    const size_t budget = limit > fixed ? limit - fixed : 0;

    const std::string fitted = truncate_from_the_left(detail, budget, ellipsis);

    std::string row = head + pad_to(fitted, std::min(DETAIL_WIDTH, budget)) + elapsed;

    // a last resort, and it fires only on a terminal too narrow for the columns at all. Trimming the
    // trailing padding first is what keeps it from ever firing on an ordinary row
    row = right_trimmed(row);
    if (display_width(row) > limit) {
        row = truncate_from_the_left(row, limit, ellipsis);
    }

    if (mark_sgr == nullptr || !_capabilities.color) {
        return row;
    }

    // the mark is coloured after the arithmetic, never before - an SGR sequence has no width and
    // measuring one is how a row that fits becomes a row that wraps
    const size_t at = row.find(mark);
    if (at == std::string::npos) {
        return row;
    }

    return row.substr(0, at) + styled(mark, mark_sgr, true) + row.substr(at + mark.size());
}

void Compiler::ProgressReporter::erase_live_row()
{
    if (!_drawn) {
        return;
    }

    *_out << ERASE_ROW << std::flush;
    _drawn = false;
}

void Compiler::ProgressReporter::draw_live_row()
{
    const std::string frame = _theme.spinner.empty()
        ? _theme.skipped
        : _theme.spinner[_frame % _theme.spinner.size()];

    *_out << ERASE_ROW
          << render_row(_live_phase.value(), _live_subject, _live_detail, frame, sgr::dim, std::nullopt)
          << std::flush;

    _drawn = true;
}

void Compiler::ProgressReporter::open(ProgressPhase phase, const std::string &subject)
{
    if (!enabled()) {
        return;
    }

    // whatever was live is closed as failed first, so a row cannot be silently lost by a caller that
    // forgot to close it - the same answer ProgressStep's destructor gives, for the same reason
    if (_live_phase.has_value()) {
        commit(ProgressState::t_failed, 0);
    }

    _live_phase = phase;
    _live_subject = subject;
    _live_detail.clear();
    _frame = 0;

    draw_live_row();
}

void Compiler::ProgressReporter::tick(const std::string &detail)
{
    if (!enabled() || !_live_phase.has_value()) {
        return;
    }

    _live_detail = detail;
    _frame++;

    draw_live_row();
}

void Compiler::ProgressReporter::set_detail(const std::string &detail)
{
    if (!enabled() || !_live_phase.has_value()) {
        return;
    }

    _live_detail = detail;
}

void Compiler::ProgressReporter::commit(
    ProgressState state,
    unsigned int milliseconds,
    const std::vector<std::filesystem::path> &details
)
{
    if (!enabled() || !_live_phase.has_value()) {
        return;
    }

    const bool failed = state == ProgressState::t_failed;
    const std::string mark = failed ? _theme.failed : _theme.done;
    const char *mark_sgr = failed ? sgr::error : sgr::success;

    // the erase is written whether or not a row is on screen, so the bytes a commit produces do not
    // depend on whether somebody suspended in between
    *_out << ERASE_ROW
          << render_row(_live_phase.value(), _live_subject, _live_detail, mark, mark_sgr, milliseconds)
          << "\n";

    // **a failed row lists nothing.** The files under a row are what it *did*, and a step that failed did
    // not do them - the diagnostic that follows on this same stream is what names the one that mattered.
    // Owned here rather than by ProgressStep, so a direct commit cannot answer it differently
    if (!failed) {
        for (const std::filesystem::path &path : details) {
            *_out << std::string(DETAIL_LINE_INDENT, ' ')
                  << styled(path.string(), sgr::dim, _capabilities.color) << "\n";
        }
    }

    *_out << std::flush;

    _live_phase.reset();
    _live_subject.clear();
    _live_detail.clear();
    _drawn = false;
}

void Compiler::ProgressReporter::row(
    ProgressPhase phase,
    const std::string &subject,
    const std::string &detail,
    ProgressState state
)
{
    if (!enabled()) {
        return;
    }

    erase_live_row();

    const bool failed = state == ProgressState::t_failed;
    const std::string mark = state == ProgressState::t_skipped
        ? _theme.skipped
        : (failed ? _theme.failed : _theme.done);

    const char *mark_sgr = state == ProgressState::t_skipped
        ? sgr::dim
        : (failed ? sgr::error : sgr::success);

    *_out << ERASE_ROW << render_row(phase, subject, detail, mark, mark_sgr, std::nullopt) << "\n"
          << std::flush;

    // a standalone row is written *between* steps, but nothing enforces that - so a live one is put back
    // rather than lost
    if (_live_phase.has_value()) {
        draw_live_row();
    }
}

void Compiler::ProgressReporter::suspend()
{
    if (!enabled()) {
        return;
    }

    erase_live_row();
}

void Compiler::ProgressReporter::close(const std::string &what, unsigned int milliseconds)
{
    if (!enabled()) {
        return;
    }

    if (_live_phase.has_value()) {
        commit(ProgressState::t_failed, milliseconds);
    }

    erase_live_row();

    *_out << "\n  " << styled(_theme.done, sgr::success, _capabilities.color) << "  "
          << fmt::format("{} in {} ms", what, milliseconds) << "\n\n"
          << std::flush;
}

Compiler::ProgressStep::ProgressStep(
    ProgressReporter &reporter,
    ProgressPhase phase,
    const std::string &subject
) :
    _reporter(reporter),
    _start(std::chrono::steady_clock::now())
{
    _reporter.open(phase, subject);
}

Compiler::ProgressStep::~ProgressStep()
{
    // nobody said it worked, so it did not. That is what covers every early return and every throw in the
    // driver without one of them naming this object
    finish(false);
}

void Compiler::ProgressStep::tick(const std::string &detail)
{
    _reporter.tick(detail);
}

void Compiler::ProgressStep::summary(std::string text)
{
    _summary = std::move(text);
}

void Compiler::ProgressStep::detail(std::vector<std::filesystem::path> paths)
{
    _details = std::move(paths);
}

void Compiler::ProgressStep::finish(bool ok)
{
    if (_closed) {
        return;
    }

    _closed = true;

    // the summary replaces whatever the last tick left in the detail column, which is a file name the
    // step has by then finished with
    _reporter.set_detail(_summary);
    _reporter.commit(
        ok ? ProgressState::t_done : ProgressState::t_failed,
        progress_elapsed_ms(_start),
        _details);
}

unsigned int Compiler::progress_elapsed_ms(std::chrono::steady_clock::time_point start)
{
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<unsigned int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}
