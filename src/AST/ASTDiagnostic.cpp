#include "AST/ASTDiagnostic.h"
#include "AST/ASTModule.h"

AST::Span AST::span_of(const TokenSlice &slice, const File *fallback_file)
{
    Span span;

    // the token names its own file. fallback is only for a token nothing stamped - a lexer-only
    // test, a mint from a no-file `at` - so the frame still has somewhere to draw
    span.file = slice.start_ref().file();
    if (span.file == nullptr) {
        span.file = fallback_file;
    }

    const Token &start = slice.startt();
    span.start = Location { start.line, start.char_offset };

    // `end_index` is inclusive - TokenSlice::is_empty() is start == end, which is a slice of one token -
    // so the range ends past the *text* of that token rather than at its offset
    const Token &end = slice.endt();
    const TokenReference end_ref = slice.end_ref();
    const uint32_t end_width =
        end_ref.is_valid() ? static_cast<uint32_t>(end_ref.value().size()) : 1;

    span.end = Location { end.line, end.char_offset + (end_width > 0 ? end_width : 1) };

    // a slice whose end sits before its start describes nothing drawable. It happens where a pass minted
    // a virtual token at a borrowed position, and collapsing to the start is what keeps the underline on
    // a real character instead of running backwards across the line
    if (span.end.line < span.start.line
        || (span.end.line == span.start.line && span.end.column < span.start.column)) {
        span.end = Location { span.start.line, span.start.column + 1 };
    }

    return span;
}

AST::Span AST::span_of(const TokenReference &token)
{
    if (!token.is_valid()) {
        return {};
    }

    return span_of(token.make_slice(), token.file());
}

AST::Diagnostic AST::to_diagnostic(const IssueRecord &issue)
{
    const CodeRef &code_ref = issue.code_ref;

    Diagnostic diagnostic;
    diagnostic.severity = issue.severity;
    diagnostic.code = issue.code();
    diagnostic.message = issue.message();
    diagnostic.module_name = code_ref.module != nullptr ? code_ref.module->name : std::string();
    diagnostic.primary = span_of(code_ref.token_slice, code_ref.file());
    diagnostic.primary_label = issue.primary_label();

    for (const auto &label : issue.labels()) {
        diagnostic.labels.push_back(
            DiagnosticLabel { span_of(label.span, code_ref.file()), label.message });
    }

    for (const auto &note : issue.notes()) {
        diagnostic.notes.push_back(DiagnosticNote { note.kind, note.message });
    }

    return diagnostic;
}
