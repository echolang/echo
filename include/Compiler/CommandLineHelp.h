#ifndef COMMANDLINEHELP_H
#define COMMANDLINEHELP_H

#pragma once

#include "Compiler/CommandLineOption.h"
#include "Compiler/TerminalCapabilities.h"

#include <ostream>
#include <string>

namespace Compiler
{
    // the connectors that hang an option's values off it. **glyphs only**, the shape AST::DiagnosticTheme
    // already has and for its reason: a Windows console draws SGR perfectly and turns box-drawing into
    // mojibake, so colour and repertoire are two answers and this is the repertoire half
    struct HelpTheme
    {
        const char *branch;
        const char *last_branch;

        // what a section heading is drawn with: a mark to its left and the rule that runs out to the
        // right of it. The plain form draws neither, so an ascii page is the words alone rather than a
        // row of hyphens pretending to be a rule
        const char *heading_mark;
        const char *heading_rule;

        static HelpTheme pretty();
        static HelpTheme ascii();
    };

    // how an option is spelled back: a page, one option in full, a usage line, a refusal.
    // the page is one line per option; `--help <option>` prints the prose. a value a
    // subcommand cannot write is shown and marked, never hidden. DiagnosticRenderer cannot
    // draw a refusal from here: it needs --color / --diagnostics, which this is parsing
    class CommandLineHelp
    {
    public:

        CommandLineHelp(std::ostream &out, TerminalCapabilities capabilities);

        // the page. `t_none` draws the overview that lists the three subcommands
        void render_help(Subcommand subject) const;

        // one option, in full - what `echoc build --help optimize` answers
        void render_option_help(Subcommand subject, const CommandLineOption &option) const;

        // the usage line or lines on their own - what a refusal is followed by
        void render_usage(Subcommand subject) const;

        // a refusal: the sentence, a blank line, the usage for whatever subcommand had been read, and the
        // line naming --help. **stderr, always**, where the page goes to stdout - the e2e corpus
        // byte-compares the merged streams, so which stream a thing is on is a contract
        void render_error(const std::string &message, Subcommand subject) const;

        // ECO_VERSION_STRING and a newline. **nothing else on the line**, ever: the release workflow
        // string-compares it against the tag it resolved, so a page opening `echoc 0.1.0 - the echo...`
        // fails that job with a message about the version rather than about the format
        void render_version() const;

    private:

        std::ostream &_out;
        const TerminalCapabilities _capabilities;
        const HelpTheme _theme;

        // Compiler::styled with this renderer's colour answer already supplied, the spelling
        // AST::DiagnosticRenderer already uses for the same reason
        std::string styled(const std::string &text, const char *sgr) const;

        // **not TerminalCapabilities::width, and the difference is deliberate.** A diagnostic wraps the
        // *user's* text, so 0 means do not wrap at all and a golden cannot depend on the window it was
        // recorded in. A help page wraps our own prose and has no other job, so a stream with no width
        // gets 80 - deterministic, and readable through a pager. Capped at 100, because a two-hundred
        // column terminal turns a paragraph into one line nobody can track back
        unsigned int page_width() const;

        // where every summary on this page starts. **one width for the whole page**, so the summaries
        // form a column that can be read down; capped, so one wide spelling cannot push the rest of the
        // page off the screen - a heading past the cap takes the line to itself instead
        unsigned int summary_column(Subcommand subject) const;

        // **help's own wrapper, and not AST's write_block.** That one bakes in a two-space indent and
        // treats width 0 as "do not wrap", which are the two things that differ here. Two callers asking
        // two questions is two functions; the day a third wrapper appears is the day a shared one is
        // worth minting, and not before
        void write_wrapped(const std::string &text, unsigned int indent) const;

        // one row of the page: a spelling, its summary at the column, and an optional right-aligned
        // marker. The one place the column and the overflow rule are applied
        void write_row(
            const std::string &spelling,
            unsigned int spelling_width,
            const std::string &summary,
            const std::string &marker,
            unsigned int column) const;

        // a section heading, drawn with whatever the terminal can render
        void render_heading(const std::string &text) const;

        void render_overview() const;
        void render_category(Subcommand subject, OptionCategory category, unsigned int column) const;
        void render_option(
            const CommandLineOption &option, Subcommand subject, unsigned int column) const;
        void render_sources(const SubcommandInfo &info, unsigned int column) const;
    };
};

#endif
