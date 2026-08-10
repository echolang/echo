#ifndef TERMINALSTYLE_H
#define TERMINALSTYLE_H

#pragma once

#include <string>

namespace Compiler
{
    // **the sole answer to "what is the escape sequence for this".**
    //
    // one table, because the failure mode is Compiler::TerminalCapabilities' own: two places spelling an
    // SGR sequence is two places for a theme change to land on one of them. These lived as file-local
    // constants in src/AST/ASTDiagnosticRenderer.cpp, which was right while the renderer was the only
    // thing that wrote to the terminal - Compiler::ProgressReporter is the second, and copying three of
    // them across would have been the copy that drifts.
    //
    // deliberately *not* a theme: AST::DiagnosticTheme and Compiler::ProgressTheme each pick their own
    // glyphs, and both reach in here for the colour. A glyph is a repertoire question and a colour is not
    namespace sgr
    {
        constexpr const char *reset = "\x1b[0m";
        constexpr const char *dim = "\x1b[2m";
        constexpr const char *bold = "\x1b[1m";
        constexpr const char *error_badge = "\x1b[1;41;97m";
        constexpr const char *warning_badge = "\x1b[1;43;30m";
        constexpr const char *info_badge = "\x1b[1;44;97m";
        constexpr const char *error = "\x1b[1;31m";
        constexpr const char *warning = "\x1b[1;33m";
        constexpr const char *info = "\x1b[1;34m";
        constexpr const char *secondary = "\x1b[1;36m";
        constexpr const char *help = "\x1b[1;36m";
        constexpr const char *success = "\x1b[1;32m";
    };

    // wraps `text` in an SGR sequence, or hands it back untouched when colour is off. **The one place an
    // escape sequence is written**, so "is colour allowed" is asked once rather than per call site - and
    // taking the answer as a parameter rather than reading a TerminalCapabilities is what lets both
    // callers keep their own copy of that struct without this knowing either of them
    std::string styled(const std::string &text, const char *sgr, bool color);
};

#endif
