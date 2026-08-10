#ifndef TERMINALCAPABILITIES_H
#define TERMINALCAPABILITIES_H

#pragma once

#include <string>

namespace Compiler
{
    // what the invocation asked for, ahead of anything the environment says. `t_auto` on both axes is
    // "work it out", which is the default and the only value that ever consults a terminal
    enum class ColorChoice
    {
        t_auto,
        t_always,
        t_never
    };

    // how a diagnostic is drawn. `t_auto` resolves to `t_pretty` or `t_ascii` by what the stream can
    // render; `t_json` is not a theme at all and is carried here because it is the same one flag to a
    // user - `--diagnostics=<auto|pretty|ascii|json>`
    enum class DiagnosticFormat
    {
        t_auto,
        t_pretty,
        t_ascii,
        t_json
    };

    // which stream the facts are about.
    //
    // **the policy below is unchanged**: everything that *reports* asks about stderr, and that is not
    // negotiable. This exists for the one thing that does not report - `--help` is an answer to a question
    // the user asked, so it goes to stdout, and a page wrapped to stderr's width when stdout is the
    // redirected one is a page wrapped to the wrong window
    enum class TerminalStream
    {
        t_stderr,
        t_stdout
    };

    // **the sole answer to "what can this terminal do".**
    //
    // four facts - colour, unicode, width, interactive - resolved once from the command line and the
    // environment, and read by everything that writes to a terminal. Shaped like Compiler::TargetFacts on
    // purpose: a small
    // struct of settled facts rather than a helper each caller asks its own way, because the failure mode
    // is the same one. Two places deciding independently whether to emit an escape sequence is how a
    // redirected stream ends up with half of them in it.
    //
    // it replaced exactly that: a cached `isatty(fileno(stdout))` in main.cpp read by one function, beside
    // a set of `SH_COLOR_*` token-pasting macros that only worked on string literals, beside a renderer
    // that emitted no colour at all because reaching those macros from src/AST/ was not possible.
    //
    // **stderr is the stream that matters**, because that is where diagnostics go. stdout under `run`
    // belongs to the program being executed, and asking whether *it* is a terminal answers a question
    // almost nobody here has - Compiler::CommandLineHelp is the single exception, and it is an exception
    // precisely because a help page is not a report.
    struct TerminalCapabilities
    {
        // may an escape sequence be written. false whenever the answer is in doubt - a stray `\x1b[31m` in
        // a log or a golden is noise somebody has to filter back out, and a missing colour is not
        bool color = false;

        // may a glyph outside ASCII be written. Separate from `color` because they fail apart: a Windows
        // console with virtual-terminal processing on renders colour perfectly and turns `━` into mojibake,
        // and a UTF-8 xterm piped into a file is the reverse
        bool unicode = false;

        // how wide the terminal is, for wrapping. 0 means "unknown, do not wrap" - which is what a pipe
        // reports, and is deliberately the same answer as "narrow enough that wrapping would not help"
        unsigned int width = 0;

        // may the cursor be moved on this stream - a carriage return and an erase-to-end-of-line, so a
        // line can be rewritten in place. **Not derivable from the other three.** `width` is 0 whenever
        // the ioctl fails on a terminal that is very much one, and `color` is forced true by
        // `--color=always`, which is a request for escape sequences in a *log* and emphatically not a
        // request for a line that rewrites itself in one.
        //
        // **the one fact here with no flag behind it**, for that same reason: there is a real invocation
        // that wants colour in a file and none that wants a carriage return in one
        bool interactive = false;

        // resolves the facts for a stream - the diagnostic one, stderr, unless another is named - applying
        // the two flags over what the environment says. The flags win outright: `--color=always` into a
        // pipe is a user asking for escape sequences on purpose, which is what a CI job that renders them
        // wants.
        //
        // `NO_COLOR` (any value) and `TERM=dumb` suppress; `CLICOLOR_FORCE` (non-empty, not "0") forces.
        // unicode is decided by the locale saying UTF-8, or by `WT_SESSION` - Windows Terminal, as opposed
        // to the legacy console the same binary may be run from.
        //
        // the three environment escape hatches are properties of the *invocation* and so are asked the
        // same way whichever stream this is about; what moves with the stream is the isatty underneath
        // each of them, and the ioctl `width` reads
        static TerminalCapabilities resolve(
            ColorChoice color_choice,
            DiagnosticFormat format,
            TerminalStream stream = TerminalStream::t_stderr
        );

        // no colour, no unicode, no width, not interactive. What every non-terminal gets, and what the
        // unit tests assert against so a suite's output does not depend on where it was run from
        static TerminalCapabilities plain();
    };

    // `auto` / `always` / `never` and `auto` / `pretty` / `ascii` / `json`, refused rather than defaulted
    // on anything else - a mistyped `--color=alwyas` that silently means `auto` is a flag the user
    // believes they set. False with a sentence in `out_error`, which is how TargetFacts::resolve reports
    // the same class of mistake
    bool parse_color_choice(const std::string &value, ColorChoice &out_choice, std::string &out_error);
    bool parse_diagnostic_format(const std::string &value, DiagnosticFormat &out_format, std::string &out_error);
};

#endif
