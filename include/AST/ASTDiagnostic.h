#ifndef ASTDIAGNOSTIC_H
#define ASTDIAGNOSTIC_H

#pragma once

#include "AST/ASTIssue.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AST
{
    // 1-based on both axes, because Token::char_offset already is and clang, gcc and rustc all report
    // that way. An editor protocol that wants 0-based converts at its own boundary - doing it here would
    // make every number the compiler prints disagree with every number it stores
    struct Location
    {
        uint32_t line = 0;
        uint32_t column = 0;
    };

    // a half-open character range. `end.column` is one past the last character, so an empty span and a
    // one-character span are distinguishable - which they have to be, because a token slice can be either
    struct Span
    {
        const File *file = nullptr;
        Location start;
        Location end;
    };

    struct DiagnosticLabel
    {
        Span span;
        std::string message;
    };

    struct DiagnosticNote
    {
        NoteKind kind = NoteKind::t_note;
        std::string message;
    };

    // **an issue, flattened into exactly what it takes to draw one.**
    //
    // the reason there is one renderer and not three. An IssueRecord is a class hierarchy holding typed
    // fields and a virtual `message()`; this is a value with no behaviour, and every consumer - the text
    // renderer, the JSON writer, and whatever an editor integration turns out to need - reads *this*.
    // Nothing downstream of here re-derives a location, re-asks a virtual, or knows what an issue kind is.
    //
    // it is also the seam a language server needs: serialising this requires the AST headers, but
    // consuming what it serialises requires nothing at all
    struct Diagnostic
    {
        IssueSeverity severity = IssueSeverity::Error;

        // the issue class's own name, or nothing where the kind is not a classification. See
        // ISSUE_CODE_OF in ASTIssue.h for why it is never invented
        std::optional<std::string> code;

        std::string message;
        std::string module_name;

        Span primary;
        std::string primary_label;

        std::vector<DiagnosticLabel> labels;
        std::vector<DiagnosticNote> notes;
    };

    // **the one place a token slice becomes a character range.** Both renderers and every future consumer
    // read the result, so the two things this gets right are got right once:
    //
    // - a `Token` carries a start and no length, so the end column is the last token's offset plus the
    //   width of its text. That is why the underline can span a whole expression rather than being the
    //   single caret the old excerpt printer drew.
    // - a **string literal stores its value without the quotes**, so its span is two characters short.
    //   Known and accepted.
    Span span_of(const TokenSlice &slice, const File *fallback_file = nullptr);

    Diagnostic to_diagnostic(const IssueRecord &issue);
};

#endif
