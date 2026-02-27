#ifndef ASTDIAGNOSTICRENDERER_H
#define ASTDIAGNOSTICRENDERER_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "Compiler/TerminalCapabilities.h"

#include <ostream>
#include <string>

namespace AST
{
    // the glyphs a frame is drawn with, and nothing else. **Two themes, one renderer** - the layout code
    // is written once against these names, so the pretty form and the CI form cannot drift into being
    // different renderings of different information. That is exactly how the compiler ended up with three
    // diagnostic channels the first time.
    //
    // no colour lives here: colour is a `TerminalCapabilities` question, asked per span by the renderer,
    // and a theme that carried escape sequences could not be reused by the ascii path
    struct DiagnosticTheme
    {
        const char *gutter;         // between the line number and the source
        const char *underline_rule; // the gutter on an underline row
        const char *primary_mark;   // under the primary span
        const char *secondary_mark; // under a secondary label's span
        const char *failure_mark;   // the summary bullet
        const char *warning_mark;

        // whether the banner rows are *decorated* - a reverse-video severity chip and a summary bullet -
        // or plain words. it is a theme answer rather than a second reading of
        // `TerminalCapabilities::unicode`: the renderer derives the theme from that flag once, in its
        // constructor, and three sites that re-asked it were three chances to derive it differently
        bool chips;

        static DiagnosticTheme pretty();
        static DiagnosticTheme ascii();
    };

    // **the sole answer to "how does a diagnostic reach a human or a tool".**
    //
    // one object, constructed once from the command line, passed to everything that reports. It replaced
    // three renderers that each knew a different amount: `AST::print_issue` (a message and a miscounted
    // caret), `report_compiler_exception` (that plus a prefix), and `print_critical_error` (a bold title
    // and no location at all, which is why a manifest error still shows no source excerpt).
    //
    // the format is a field rather than a subclass, because `json` is not a different *layout* of the
    // same thing - it is the same Diagnostic serialised instead of drawn, and a virtual `render` split
    // across two classes would have put the "which fields exist" question in two places again.
    //
    // **everything goes to the stream given at construction, which is stderr.** stdout under `run`
    // belongs to the program being executed, and a compiler that writes into it is a compiler whose
    // output cannot be piped
    class DiagnosticRenderer
    {
    public:

        DiagnosticRenderer(
            std::ostream &out,
            Compiler::DiagnosticFormat format,
            Compiler::TerminalCapabilities capabilities);

        void render(const Diagnostic &diagnostic) const;

        void render_issue(const IssueRecord &issue) const {
            render(to_diagnostic(issue));
        }

        // the tail line - `1 error, nothing was compiled.` - and, under json, the one summary object that
        // tells a reader the stream ended rather than was cut off
        void render_summary(size_t errors, size_t warnings, bool compiled) const;

        // an issue with no source behind it: a broken manifest, a failed tokenization, an unusable
        // target. It gets the badge and the same stream, and deliberately **not** an invented location -
        // making these carry one is a job of its own, and faking it here would remove the reason to do it
        //
        // **the severity is a parameter and defaults to Error** because not every locationless issue
        // stops the compile: `-g` on `run` is a flag that cannot be honoured by a subcommand that still
        // succeeds, and rendering that as an error puts an Error object in the json stream of a build
        // that worked. A non-Error caller is therefore *not* the last thing printed, so the "no blank
        // line after" note below is the terminal callers' property rather than this function's
        void render_untyped(
            const std::string &title,
            const std::string &message,
            IssueSeverity severity = IssueSeverity::Error) const;

        bool is_machine_readable() const {
            return _format == Compiler::DiagnosticFormat::t_json;
        }

    private:

        std::ostream &_out;
        const Compiler::DiagnosticFormat _format;
        const Compiler::TerminalCapabilities _capabilities;
        const DiagnosticTheme _theme;

        // wraps `text` in an SGR sequence, or hands it back untouched when colour is off. The one place
        // an escape sequence is written, so "is colour allowed" is asked once rather than per call site
        std::string styled(const std::string &text, const char *sgr) const;

        // the severity chip a banner row opens with - `render_header`'s and `render_untyped`'s, which
        // are the same shape and were built twice. one answer, so a badge change cannot land on the
        // diagnostics and miss the five locationless failures
        std::string badge(IssueSeverity severity) const;

        void render_text(const Diagnostic &diagnostic) const;
        void render_json(const Diagnostic &diagnostic) const;

        // the header row: badge, code, and the location flush against the source text
        void render_header(const Diagnostic &diagnostic) const;

        // the source frame for one span, with `label` beside the underline. Used for the primary span and
        // for every secondary label, which is what makes a second location look like a first one
        void render_frame(
            const Span &span, const std::string &label, IssueSeverity severity, bool is_primary) const;
    };
};

#endif
