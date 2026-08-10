#include "AST/ASTDiagnosticRenderer.h"

#include "Compiler/ProgressReporter.h"
#include "Compiler/TerminalStyle.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstring>

namespace
{
    // a tab in a source line makes every column arithmetic lie, so the renderer expands them and moves the
    // underline by the same amount. Four, because that is what the Echo sources use and a renderer that
    // guessed eight would draw the underline in the wrong place in this very repository
    constexpr unsigned int TAB_WIDTH = 4;

    // how many source lines a single span may show before it is folded. A span that covers a whole
    // function body is a real thing - an ownership refusal names the scope - and printing eighty lines to
    // say one is what makes a reader stop reading diagnostics
    constexpr uint32_t MAX_SPAN_LINES = 8;

    std::string expand_tabs(const std::string &line)
    {
        std::string expanded;
        for (const char c : line) {
            if (c != '\t') {
                expanded += c;
                continue;
            }

            do {
                expanded += ' ';
            } while (expanded.size() % TAB_WIDTH != 0);
        }

        return expanded;
    }

    // a 1-based source column, as a 1-based column into expand_tabs(line). Walks the same loop the
    // expansion does rather than scaling, because a tab advances to the next stop and not by a fixed width
    uint32_t visual_column(const std::string &line, uint32_t column)
    {
        if (column == 0) {
            return 1;
        }

        size_t visual = 0;
        for (size_t i = 0; i + 1 < column && i < line.size(); i++) {
            if (line[i] == '\t') {
                do {
                    visual++;
                } while (visual % TAB_WIDTH != 0);
            }
            else {
                visual++;
            }
        }

        return static_cast<uint32_t>(visual) + 1;
    }

    unsigned int digit_count(uint32_t value)
    {
        unsigned int digits = 1;
        while (value >= 10) {
            value /= 10;
            digits++;
        }

        return digits;
    }

    std::string repeat(const char *unit, size_t times)
    {
        std::string out;
        out.reserve(std::strlen(unit) * times);
        for (size_t i = 0; i < times; i++) {
            out += unit;
        }

        return out;
    }

    // everything a severity decides, in one place. three parallel switches over one three-valued enum
    // is three places to find when a severity is added, and the one that gets missed reports as an
    // error - so the word and the two escape sequences are looked up together or not at all
    struct SeverityStyle
    {
        const char *word;
        const char *sgr;
        const char *badge_sgr;
    };

    SeverityStyle style_of(AST::IssueSeverity severity)
    {
        switch (severity) {
        case AST::IssueSeverity::Error:
            return SeverityStyle { "error", Compiler::sgr::error, Compiler::sgr::error_badge };
        case AST::IssueSeverity::Warning:
            return SeverityStyle { "warning", Compiler::sgr::warning, Compiler::sgr::warning_badge };
        case AST::IssueSeverity::Info:
            return SeverityStyle { "info", Compiler::sgr::info, Compiler::sgr::info_badge };
        }

        return SeverityStyle { "error", Compiler::sgr::error, Compiler::sgr::error_badge };
    }

    // the shortest thing that still names the file. **The basename and not the path**, matching what
    // `die` and `assert` already print at runtime, and the only spelling a golden can hold: the test
    // harness hands echoc an absolute path and the tests binary's working directory is deliberately not
    // fixed, so anything longer is machine-specific. The json form emits the full path, because a tool has
    // to be able to open it
    std::string file_label(const AST::File *file)
    {
        if (file == nullptr) {
            return "<unknown>";
        }

        return file->get_path().filename().string();
    }

    // a message at a fixed indent, wrapping at `width` where one is known. Several kinds already hand
    // over more than one line - an overload set lists its candidates - so the split is not an extra
    // feature, it is what keeps the second line at the same indent as the first.
    //
    // **`width` is 0 whenever the stream is not a terminal**, and 0 means no wrapping at all, which is
    // what keeps a golden from depending on the window it was recorded in
    void write_block(std::ostream &out, const std::string &text, unsigned int width)
    {
        const std::string indent = "  ";
        const size_t usable = width > indent.size() + 8 ? width - indent.size() : 0;

        size_t start = 0;
        while (start <= text.size()) {
            const size_t newline = text.find('\n', start);
            const size_t length = newline == std::string::npos ? text.size() - start : newline - start;
            std::string line = text.substr(start, length);

            while (usable > 0 && line.size() > usable) {
                size_t split = line.rfind(' ', usable);
                if (split == std::string::npos || split == 0) {
                    split = usable;
                }

                out << indent << line.substr(0, split) << "\n";
                line = line.substr(line[split] == ' ' ? split + 1 : split);
            }

            out << indent << line << "\n";

            if (newline == std::string::npos) {
                break;
            }

            start = newline + 1;
        }
    }

    void write_json_string(std::ostream &out, const std::string &value)
    {
        out << '"';
        for (const unsigned char c : value) {
            switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                // everything below 0x20 has to be escaped; everything above is passed through as the
                // bytes it already is, which keeps UTF-8 in a message intact rather than mangling it into
                // \u escapes of individual bytes
                if (c < 0x20) {
                    out << fmt::format("\\u{:04x}", static_cast<unsigned int>(c));
                }
                else {
                    out << static_cast<char>(c);
                }
                break;
            }
        }
        out << '"';
    }

    void write_json_span(std::ostream &out, const AST::Span &span)
    {
        out << "{\"file\":";
        if (span.file != nullptr) {
            write_json_string(out, span.file->get_path().string());
        }
        else {
            out << "null";
        }

        out << fmt::format(
            ",\"start\":{{\"line\":{},\"column\":{}}},\"end\":{{\"line\":{},\"column\":{}}}}}",
            span.start.line,
            span.start.column,
            span.end.line,
            span.end.column
        );
    }
};

AST::DiagnosticTheme AST::DiagnosticTheme::pretty()
{
    return DiagnosticTheme { "│", "·", "━", "─", "✗", "⚠", true };
}

AST::DiagnosticTheme AST::DiagnosticTheme::ascii()
{
    return DiagnosticTheme { "|", ":", "^", "-", "x", "!", false };
}

AST::DiagnosticRenderer::DiagnosticRenderer(
    std::ostream &out,
    Compiler::DiagnosticFormat format,
    Compiler::TerminalCapabilities capabilities
) :
    _out(out),
    _format(format),
    _capabilities(capabilities),
    _theme(capabilities.unicode ? DiagnosticTheme::pretty() : DiagnosticTheme::ascii())
{}

std::string AST::DiagnosticRenderer::styled(const std::string &text, const char *sgr) const
{
    return Compiler::styled(text, sgr, _capabilities.color);
}

std::string AST::DiagnosticRenderer::badge(IssueSeverity severity) const
{
    const SeverityStyle style = style_of(severity);

    if (!_theme.chips) {
        return std::string("[") + style.word + "]";
    }

    // a reverse-video chip, the shape every modern task runner uses for a status. Padded with spaces
    // *inside* the styling so the background is a block rather than a word
    std::string word = style.word;
    std::transform(word.begin(), word.end(), word.begin(), ::toupper);

    return "  " + styled(" " + word + " ", style.badge_sgr);
}

void AST::DiagnosticRenderer::render(const Diagnostic &diagnostic) const
{
    Compiler::ProgressReporter::instance().suspend();

    if (is_machine_readable()) {
        render_json(diagnostic);
        return;
    }

    render_text(diagnostic);
}

void AST::DiagnosticRenderer::render_header(const Diagnostic &diagnostic) const
{
    const std::string location = fmt::format(
        "{}:{}:{}", file_label(diagnostic.primary.file),
        diagnostic.primary.start.line, diagnostic.primary.start.column);

    std::string line = badge(diagnostic.severity) + " ";
    if (diagnostic.code.has_value()) {
        line += styled(*diagnostic.code, Compiler::sgr::bold) + "  ";
    }

    line += styled(location, Compiler::sgr::dim);

    _out << line << "\n\n";
}

void AST::DiagnosticRenderer::render_frame(
    const Span &span,
    const std::string &label,
    IssueSeverity severity,
    bool is_primary
) const
{
    const File *file = span.file;
    if (file == nullptr || !file->content.has_value()) {
        _out << "  " << styled("<source unavailable>", Compiler::sgr::dim) << "\n\n";
        return;
    }

    const uint32_t total_lines = static_cast<uint32_t>(file->line_count());
    const uint32_t first_context = span.start.line > 1 ? span.start.line - 1 : 1;
    const uint32_t last_context = std::min(span.end.line + 1, total_lines);

    const unsigned int gutter_width = digit_count(last_context);
    const std::string blank_gutter = std::string(gutter_width, ' ');

    const char *mark_sgr = is_primary ? style_of(severity).sgr : Compiler::sgr::secondary;
    const char *mark_glyph = is_primary ? _theme.primary_mark : _theme.secondary_mark;

    // folded when the span is taller than a reader will follow. The elision row says how many lines went,
    // because a frame that silently skips is a frame that misrepresents where the span ends
    const bool folds = span.end.line - span.start.line + 1 > MAX_SPAN_LINES;

    for (uint32_t line = first_context; line <= last_context; line++) {
        if (folds && line > span.start.line + 2 && line < span.end.line - 1) {
            if (line == span.start.line + 3) {
                _out << "  " << blank_gutter << " " << styled(_theme.underline_rule, Compiler::sgr::dim) << " "
                     << styled(fmt::format("... {} lines", span.end.line - span.start.line - 4), Compiler::sgr::dim)
                     << "\n";
            }
            continue;
        }

        const std::string source = file->get_content_of_line(line);
        const std::string expanded = expand_tabs(source);

        const bool in_span = line >= span.start.line && line <= span.end.line;

        _out << "  " << styled(fmt::format("{:>{}}", line, gutter_width), Compiler::sgr::dim) << " "
             << styled(_theme.gutter, Compiler::sgr::dim) << " " << expanded << "\n";

        if (!in_span) {
            continue;
        }

        // a multi-line span underlines each of its lines from where it enters to where it leaves, so the
        // shape of the region is visible rather than only its first character
        const uint32_t from = line == span.start.line ? visual_column(source, span.start.column) : 1;
        const uint32_t to = line == span.end.line
            ? visual_column(source, span.end.column)
            : static_cast<uint32_t>(expanded.size()) + 1;

        const size_t marks = to > from ? to - from : 1;

        std::string underline = std::string(from > 0 ? from - 1 : 0, ' ')
            + styled(repeat(mark_glyph, marks), mark_sgr);

        // the label sits on the last underlined row, where a reader's eye already is
        if (!label.empty() && line == span.end.line) {
            underline += " " + styled(label, mark_sgr);
        }

        _out << "  " << blank_gutter << " " << styled(_theme.underline_rule, Compiler::sgr::dim) << " "
             << underline << "\n";
    }

    _out << "\n";
}

void AST::DiagnosticRenderer::render_text(const Diagnostic &diagnostic) const
{
    render_header(diagnostic);

    write_block(_out, diagnostic.message, _capabilities.width);
    _out << "\n";

    render_frame(diagnostic.primary, diagnostic.primary_label, diagnostic.severity, true);

    for (const auto &label : diagnostic.labels) {
        render_frame(label.span, label.message, diagnostic.severity, false);
    }

    for (const auto &note : diagnostic.notes) {
        const char *word = note.kind == NoteKind::t_help ? "help" : "note";
        _out << "  " << styled(word, Compiler::sgr::help) << ": " << note.message << "\n";
    }

    if (!diagnostic.notes.empty()) {
        _out << "\n";
    }
}

void AST::DiagnosticRenderer::render_json(const Diagnostic &diagnostic) const
{
    _out << "{\"type\":\"diagnostic\",\"severity\":\"" << style_of(diagnostic.severity).word << "\",\"code\":";
    if (diagnostic.code.has_value()) {
        write_json_string(_out, *diagnostic.code);
    }
    else {
        _out << "null";
    }

    _out << ",\"message\":";
    write_json_string(_out, diagnostic.message);

    _out << ",\"module\":";
    write_json_string(_out, diagnostic.module_name);

    _out << ",\"span\":";
    write_json_span(_out, diagnostic.primary);

    _out << ",\"label\":";
    if (diagnostic.primary_label.empty()) {
        _out << "null";
    }
    else {
        write_json_string(_out, diagnostic.primary_label);
    }

    _out << ",\"labels\":[";
    for (size_t i = 0; i < diagnostic.labels.size(); i++) {
        if (i > 0) {
            _out << ",";
        }

        _out << "{\"span\":";
        write_json_span(_out, diagnostic.labels[i].span);
        _out << ",\"message\":";
        write_json_string(_out, diagnostic.labels[i].message);
        _out << "}";
    }

    _out << "],\"notes\":[";
    for (size_t i = 0; i < diagnostic.notes.size(); i++) {
        if (i > 0) {
            _out << ",";
        }

        _out << "{\"kind\":\"" << (diagnostic.notes[i].kind == NoteKind::t_help ? "help" : "note")
             << "\",\"message\":";
        write_json_string(_out, diagnostic.notes[i].message);
        _out << "}";
    }

    _out << "]}\n";
}

void AST::DiagnosticRenderer::render_summary(size_t errors, size_t warnings, bool compiled) const
{
    Compiler::ProgressReporter::instance().suspend();

    if (is_machine_readable()) {
        _out << fmt::format(
            "{{\"type\":\"summary\",\"errors\":{},\"warnings\":{},\"compiled\":{}}}\n",
            errors,
            warnings,
            compiled ? "true" : "false"
        );
        return;
    }

    if (errors == 0 && warnings == 0) {
        return;
    }

    std::string counts;
    if (errors > 0) {
        counts += fmt::format("{} error{}", errors, errors == 1 ? "" : "s");
    }

    if (warnings > 0) {
        if (!counts.empty()) {
            counts += ", ";
        }

        counts += fmt::format("{} warning{}", warnings, warnings == 1 ? "" : "s");
    }

    std::string line = counts;
    if (_theme.chips) {
        const char *mark = errors > 0 ? _theme.failure_mark : _theme.warning_mark;
        const char *sgr = errors > 0 ? Compiler::sgr::error : Compiler::sgr::warning;

        line = "  " + styled(mark, sgr) + "  " + counts;
    }

    if (!compiled) {
        line += ", nothing was compiled";
    }

    _out << line << ".\n";
}

void AST::DiagnosticRenderer::render_untyped(
    const std::string &title,
    const std::string &message,
    IssueSeverity severity
) const
{
    Compiler::ProgressReporter::instance().suspend();

    if (is_machine_readable()) {
        Diagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.message = title + ": " + message;
        render_json(diagnostic);
        return;
    }

    _out << badge(severity) << " " << styled(title, Compiler::sgr::bold) << "\n\n";

    // the message may already be several lines - a manifest reader hands over one line per refusal - and
    // each of them belongs at the same indent as a diagnostic's message.
    //
    // **no blank line after it**, unlike render_text: this is the last thing any of its five callers
    // prints before returning, where a diagnostic is followed by the summary that separates it
    write_block(_out, message, _capabilities.width);
}
