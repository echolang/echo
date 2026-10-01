#include "Compiler/ProgressReporter.h"

#include "Compiler/TerminalStyle.h"

#include <fmt/core.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

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
    constexpr size_t ERASE_ROW_SIZE = 4;

    // how often the clock overwrites the glyph and the elapsed digits. Short enough to read as motion,
    // long enough that a redraw is not the thing the compile is waiting on
    constexpr auto CLOCK_INTERVAL = std::chrono::milliseconds(80);

    // pre-sized on the driver at enable, so the clock never grows a string. A live row is one terminal
    // line plus SGR around the mark; 4 KiB is far past that
    constexpr size_t CLOCK_SCRATCH = 4096;

    // `{:>6} ms` as render_row spells it - six digits, a space, `ms`. The clock overwrites only the
    // digits, so this width is load-bearing
    constexpr unsigned int ELAPSED_CAP = 999999;

#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

    void write_fd(int fd, const char *data, size_t n)
    {
#if defined(_WIN32)
        _write(fd, data, static_cast<unsigned int>(n));
#else
        const ssize_t written = write(fd, data, n);
        (void)written;
#endif
    }

    // right-aligned decimal into six ASCII bytes, spaces on the left. The clock's paint, so no
    // `fmt` and no temporaries
    void write_digits6(char *at, unsigned int value)
    {
        if (value > ELAPSED_CAP) {
            value = ELAPSED_CAP;
        }

        char digits[MILLISECONDS_WIDTH];
        size_t n = 0;
        unsigned int rest = value;
        do {
            digits[n++] = static_cast<char>('0' + (rest % 10));
            rest /= 10;
        } while (rest != 0 && n < MILLISECONDS_WIDTH);

        const size_t spaces = MILLISECONDS_WIDTH - n;
        for (size_t i = 0; i < spaces; i++) {
            at[i] = ' ';
        }
        for (size_t i = 0; i < n; i++) {
            at[spaces + i] = digits[n - 1 - i];
        }
    }

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
    case ProgressPhase::t_test:
        return "test";
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

struct Compiler::ProgressReporter::Clock
{
    std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    bool stop = false;
    bool abandoned = false;
    int fd = -1;
    std::string scratch;
    size_t length = 0;
    size_t glyph_offset = 0;
    size_t glyph_bytes = 0;
    size_t ms_offset = 0;
    size_t frame = 0;
    std::chrono::steady_clock::time_point started;
    std::vector<const char *> spinner;

    std::unique_lock<std::mutex> lock();
    void halt();
    void start();
    void present(const RenderedRow &rendered);
    void pulse();
    void clear();

private:

    void run();
};

std::unique_lock<std::mutex> Compiler::ProgressReporter::Clock::lock()
{
    return std::unique_lock<std::mutex>(mutex);
}

void Compiler::ProgressReporter::Clock::halt()
{
    if (!thread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> held(mutex);
        stop = true;
    }

    cv.notify_one();

    try {
        thread.join();
    }
    catch (...) {
        // throwing here would terminate from ProgressStep's destructor. a joinable
        // thread's destructor also terminates, so detach if the join did not take
        // ownership - and then this Clock must not be destroyed while the thread
        // may still touch it
        if (thread.joinable()) {
            try {
                thread.detach();
            }
            catch (...) {
            }
        }

        abandoned = true;
    }
}

void Compiler::ProgressReporter::Clock::start()
{
    if (thread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> held(mutex);
        stop = false;
    }

    thread = std::thread([this] {
        run();
    });
}

void Compiler::ProgressReporter::Clock::present(const RenderedRow &rendered)
{
    const size_t need = ERASE_ROW_SIZE + rendered.text.size();
    if (scratch.size() < need) {
        scratch.resize(need);
    }

    std::memcpy(scratch.data(), ERASE_ROW, ERASE_ROW_SIZE);
    std::memcpy(scratch.data() + ERASE_ROW_SIZE, rendered.text.data(), rendered.text.size());
    length = need;
    glyph_bytes = rendered.glyph_bytes;
    glyph_offset = rendered.glyph_bytes == 0
        ? 0
        : ERASE_ROW_SIZE + rendered.glyph_offset;
    // 0 is "no slot". pulse must not paint at offset 0: that is ERASE_ROW
    ms_offset = rendered.ms_offset == 0
        ? 0
        : ERASE_ROW_SIZE + rendered.ms_offset;
}

void Compiler::ProgressReporter::Clock::clear()
{
    length = 0;
}

void Compiler::ProgressReporter::Clock::pulse()
{
    if (length == 0) {
        return;
    }

    if (!spinner.empty()
        && glyph_bytes > 0
        && glyph_offset + glyph_bytes <= length) {
        const char *glyph = spinner[frame % spinner.size()];
        if (std::strlen(glyph) == glyph_bytes) {
            std::memcpy(scratch.data() + glyph_offset, glyph, glyph_bytes);
        }
    }

    if (ms_offset != 0 && ms_offset + MILLISECONDS_WIDTH <= length) {
        write_digits6(scratch.data() + ms_offset, progress_elapsed_ms(started));
    }

    write_fd(fd, scratch.data(), length);
}

void Compiler::ProgressReporter::Clock::run()
{
    while (true) {
        std::unique_lock<std::mutex> held(mutex);
        if (cv.wait_for(held, CLOCK_INTERVAL, [this] {
            return stop;
        })) {
            return;
        }

        if (length == 0) {
            continue;
        }

        frame++;
        pulse();
    }
}

Compiler::ProgressReporter::ProgressReporter() = default;

Compiler::ProgressReporter::ProgressReporter(std::ostream &out, TerminalCapabilities capabilities)
{
    enable(out, capabilities);
}

Compiler::ProgressReporter::~ProgressReporter()
{
    halt_spinner();
}

Compiler::ProgressReporter &Compiler::ProgressReporter::instance()
{
    static ProgressReporter reporter;
    return reporter;
}

void Compiler::ProgressReporter::enable(
    std::ostream &out,
    TerminalCapabilities capabilities,
    bool animate,
    int clock_fd
)
{
    halt_spinner();

    _out = &out;
    _capabilities = capabilities;
    _animate = animate;

    // the theme is derived once, here, rather than per row - the rule AST::DiagnosticRenderer already
    // follows, and for its reason: three sites re-asking `unicode` are three chances to answer it
    // differently
    _theme = capabilities.unicode ? ProgressTheme::pretty() : ProgressTheme::ascii();

    if (!animate) {
        _clock.reset();
        return;
    }

    _clock = std::make_unique<Clock>();
    _clock->fd = clock_fd >= 0 ? clock_fd : STDERR_FILENO;
    _clock->scratch.assign(CLOCK_SCRATCH, '\0');
    _clock->spinner = _theme.spinner;
}

Compiler::ProgressReporter::RenderedRow Compiler::ProgressReporter::render_row(
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
    const std::string padded_detail = pad_to(fitted, std::min(DETAIL_WIDTH, budget));

    std::string row = head + padded_detail + elapsed;

    RenderedRow rendered;
    rendered.glyph_offset = 2;
    rendered.glyph_bytes = mark.size();
    rendered.ms_offset = elapsed.empty() ? 0 : head.size() + padded_detail.size();

    // a last resort, and it fires only on a terminal too narrow for the columns at all. Trimming the
    // trailing padding first is what keeps it from ever firing on an ordinary row. The layout is gone
    // after that cut, so the clock must not paint slots that no longer exist
    row = right_trimmed(row);
    if (display_width(row) > limit) {
        row = truncate_from_the_left(row, limit, ellipsis);
        rendered.glyph_offset = 0;
        rendered.glyph_bytes = 0;
        rendered.ms_offset = 0;
    } else if (mark_sgr != nullptr && _capabilities.color) {
        // the mark is coloured after the arithmetic, never before - an SGR sequence has no width and
        // measuring one is how a row that fits becomes a row that wraps
        const std::string wrapped = styled(mark, mark_sgr, true);
        const size_t extra = wrapped.size() - mark.size();
        row = row.substr(0, rendered.glyph_offset) + wrapped
            + row.substr(rendered.glyph_offset + rendered.glyph_bytes);
        rendered.glyph_offset += std::strlen(mark_sgr);
        if (rendered.ms_offset != 0) {
            rendered.ms_offset += extra;
        }
    }

    rendered.text = std::move(row);
    return rendered;
}

void Compiler::ProgressReporter::erase_live_row()
{
    if (!_drawn) {
        return;
    }

    *_out << ERASE_ROW << std::flush;
    _drawn = false;
}

void Compiler::ProgressReporter::halt_spinner()
{
    if (_clock == nullptr) {
        return;
    }

    _clock->halt();
    if (_clock->abandoned) {
        _clock.release();
    }
}

void Compiler::ProgressReporter::continue_spinner()
{
    if (!_animate || _clock == nullptr || !_drawn || !_live_phase.has_value()) {
        return;
    }

    _clock->start();
}

Compiler::ProgressReporter::ClockPause::ClockPause(ProgressReporter &reporter) :
    _reporter(reporter)
{
    _reporter.halt_spinner();
}

Compiler::ProgressReporter::ClockPause::~ClockPause()
{
    try {
        _reporter.continue_spinner();
    }
    catch (...) {
    }
}

std::unique_lock<std::mutex> Compiler::ProgressReporter::lock_clock()
{
    if (_clock == nullptr) {
        return std::unique_lock<std::mutex>();
    }

    return _clock->lock();
}

void Compiler::ProgressReporter::write_live_row()
{
    const size_t spinner_frame = _clock != nullptr ? _clock->frame : _frame;
    const std::string mark = _theme.spinner.empty()
        ? _theme.skipped
        : _theme.spinner[spinner_frame % _theme.spinner.size()];

    std::optional<unsigned int> elapsed;
    if (_animate) {
        unsigned int ms = progress_elapsed_ms(_live_started);
        if (ms > ELAPSED_CAP) {
            ms = ELAPSED_CAP;
        }
        elapsed = ms;
    }

    const RenderedRow rendered = render_row(_live_phase.value(), _live_subject, _live_detail, mark, sgr::dim, elapsed);

    *_out << ERASE_ROW << rendered.text << std::flush;
    _drawn = true;

    if (_clock != nullptr) {
        _clock->present(rendered);
    }
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

    {
        std::unique_lock<std::mutex> lock = lock_clock();
        _live_phase = phase;
        _live_subject = subject;
        _live_detail.clear();
        _live_started = std::chrono::steady_clock::now();
        if (_clock != nullptr) {
            _clock->frame = 0;
            _clock->started = _live_started;
        } else {
            _frame = 0;
        }
        write_live_row();
    }

    continue_spinner();
}

void Compiler::ProgressReporter::tick(const std::string &detail)
{
    if (!enabled() || !_live_phase.has_value()) {
        return;
    }

    {
        std::unique_lock<std::mutex> lock = lock_clock();
        _live_detail = detail;
        if (_clock != nullptr) {
            _clock->frame++;
        } else {
            _frame++;
        }
        write_live_row();
    }

    continue_spinner();
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

    std::unique_lock<std::mutex> lock = lock_clock();
    if (_clock != nullptr) {
        _clock->clear();
    }

    const bool failed = state == ProgressState::t_failed;
    const std::string mark = failed ? _theme.failed : _theme.done;
    const char *mark_sgr = failed ? sgr::error : sgr::success;

    // the erase is written whether or not a row is on screen, so the bytes a commit produces do not
    // depend on whether somebody suspended in between
    const RenderedRow finished = render_row(_live_phase.value(), _live_subject, _live_detail, mark, mark_sgr, milliseconds);

    *_out << ERASE_ROW << finished.text << "\n";

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

    {
        std::unique_lock<std::mutex> lock = lock_clock();
        if (_clock != nullptr) {
            _clock->clear();
        }

        erase_live_row();

        const bool failed = state == ProgressState::t_failed;
        const std::string mark = state == ProgressState::t_skipped
            ? _theme.skipped
            : (failed ? _theme.failed : _theme.done);

        const char *mark_sgr = state == ProgressState::t_skipped
            ? sgr::dim
            : (failed ? sgr::error : sgr::success);

        *_out << ERASE_ROW << render_row(phase, subject, detail, mark, mark_sgr, std::nullopt).text
              << "\n" << std::flush;

        // a standalone row is written *between* steps, but nothing enforces that - so a live one is put
        // back rather than lost
        if (_live_phase.has_value()) {
            write_live_row();
        }
    }

    continue_spinner();
}

void Compiler::ProgressReporter::suspend()
{
    if (!enabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_suspend);
    halt_spinner();
    erase_live_row();
}

void Compiler::ProgressReporter::close(
    const std::string &what,
    unsigned int milliseconds,
    ProgressState state
)
{
    if (!enabled()) {
        return;
    }

    halt_spinner();

    if (_live_phase.has_value()) {
        commit(ProgressState::t_failed, milliseconds);
    }

    erase_live_row();

    // the mark states the outcome of what is being closed rather than of the closing. A compile that
    // produced a binary and a test run in which one test failed are both "over", and signing the second with
    // a `✓` would make the summary disagree with the rows above it
    const bool ok = state != ProgressState::t_failed;

    *_out << "\n  "
          << styled(ok ? _theme.done : _theme.failed, ok ? sgr::success : sgr::error,
                _capabilities.color)
          << "  " << fmt::format("{} in {} ms", what, milliseconds) << "\n\n"
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
    // driver without one of them naming this object. A destructor must not throw: the clock's join and
    // the iostream write both sit in `finish`, and an exception already in flight would then terminate
    // with the live row still on the line
    try {
        finish(false);
    }
    catch (...) {
    }
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
