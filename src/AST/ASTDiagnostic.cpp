#include "AST/ASTDiagnostic.h"
#include "AST/ASTModule.h"

AST::Span AST::span_of(const Module *module, const File *fallback_file, const TokenSlice &slice)
{
    Span span;

    // a virtual token belongs to no file, and neither does a slice from a module the issue was not built
    // against. falling back to the file the diagnostic is already talking about is the honest answer:
    // the alternative is naming a file the token is not in
    span.file = fallback_file;
    if (module != nullptr) {
        if (const File *owner = module->file_of(slice.start_ref())) {
            span.file = owner;
        }
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

AST::Diagnostic AST::to_diagnostic(const IssueRecord &issue)
{
    const CodeRef &code_ref = issue.code_ref;

    Diagnostic diagnostic;
    diagnostic.severity = issue.severity;
    diagnostic.code = issue.code();
    diagnostic.message = issue.message();
    diagnostic.module_name = code_ref.module != nullptr ? code_ref.module->name : std::string();
    diagnostic.primary = span_of(code_ref.module, code_ref.file, code_ref.token_slice);
    diagnostic.primary_label = issue.primary_label();

    for (const auto &label : issue.labels()) {
        diagnostic.labels.push_back(
            DiagnosticLabel { span_of(code_ref.module, code_ref.file, label.span), label.message });
    }

    for (const auto &note : issue.notes()) {
        diagnostic.notes.push_back(DiagnosticNote { note.kind, note.message });
    }

    return diagnostic;
}
