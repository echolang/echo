#ifndef COMMANDLINE_H
#define COMMANDLINE_H

#pragma once

#include "Compiler/CommandLineOption.h"
#include "Compiler/TerminalCapabilities.h"

#include <string>
#include <vector>

namespace Compiler
{
    // stated argv: what the words were. DriverOptions is what they mean. `--help` and `--version`
    // are answers, not exits
    class CommandLine
    {
    public:

        Subcommand subcommand = Subcommand::t_none;

        // the `.eco` files and globs written as positionals, in order. **recognised at any position** -
        // the parser this replaced declared them "remaining", so a flag written after a filename was read
        // as another filename and the mistake surfaced as a missing source
        std::vector<std::string> sources;

        // everything after the first bare `--`. 'run' hands it to the JIT'd program; the other two refuse
        // a non-empty one rather than ignoring it
        std::vector<std::string> program_arguments;

        bool wants_help = false;
        bool wants_version = false;

        // which option `--help <option>` asked about, or null for the whole page.
        //
        // **read out of the positionals rather than as a value of `--help`**, which is what keeps that
        // flag a flag: `echoc build --help optimize` already parses `optimize` as a positional, so there
        // is nothing to look ahead for and no arity to special-case.
        //
        // a leftover word that names no option leaves this null and draws the page, deliberately: a
        // person who typed `echoc run --help main.eco` asked for help, and refusing them is the
        // second-worst thing this command line could do with `--help`
        const CommandLineOption *help_topic = nullptr;

        // was this option written at all, as opposed to answered from its row's fallback. **only
        // --optimize needs the difference today** and it needs it badly: '-g' means '--optimize none'
        // unless the flag was stated, so "it defaulted to module" and "the user asked for module" are two
        // answers a value alone cannot tell apart
        bool stated(Opt id) const;

        // a t_flag option: was it given
        bool flag(Opt id) const;

        // a t_value option: the word, or the row's fallback
        const std::string &value(Opt id) const;

        // a t_repeated_value option: every word, in the order written
        const std::vector<std::string> &list(Opt id) const;

        // the two spellings over the value lists. a repeatable one answers "was this among them", which
        // is what makes '-p ast -p ir' two dumps rather than the last one
        bool prints(PrintKind what) const;
        bool explains(ExplainKind what) const;

        // **as stated.** the '-g' implication is Compiler::DriverOptions' and lives nowhere else
        OptimizeMode optimize() const;

        // what a refusal may be drawn with. **answered from whatever was read before the failure** -
        // parse_command_line fills values as it goes, so a bad value for one flag still gets the colour
        // the invocation asked for. A pre-pass that scanned argv for these two first would be a second
        // parser, which is the thing this file exists to have exactly one of
        ColorChoice color_choice() const;
        DiagnosticFormat diagnostic_format() const;

    private:

        struct Given
        {
            bool stated = false;
            std::vector<std::string> words;
        };

        // indexed by size_t(Opt), sized from command_line_options()
        std::vector<Given> _given;

        const Given &given(Opt id) const;
        bool holds_value(Opt id, const char *word) const;

        friend bool parse_command_line(
            int argc,
            const char *const *argv,
            CommandLine &out,
            std::string &out_error);
    };

    // argv into values. false with a sentence. `out` is filled as far as it got, even on a refusal
    bool parse_command_line(
        int argc,
        const char *const *argv,
        CommandLine &out,
        std::string &out_error);
};

#endif
