#ifndef COMMANDLINEOPTION_H
#define COMMANDLINEOPTION_H

#pragma once

#include <string>
#include <vector>

namespace Compiler
{
    // which subcommand an invocation named. `t_none` is both "not read yet" and the honest answer for
    // `echoc --help`, which is a real invocation rather than a mistake - so a help page and a usage line
    // both have a subject before a subcommand exists
    enum class Subcommand
    {
        t_none,
        t_run,
        t_build,
        t_test,
        t_clean
    };

    // the bits an option's `subcommands` mask carries. a mask rather than a vector because the only
    // question ever asked of it is membership - once per argv word, and once per option per help page
    namespace accepts
    {
        constexpr unsigned int run = 1u << 0;
        constexpr unsigned int build = 1u << 1;
        constexpr unsigned int test = 1u << 2;
        constexpr unsigned int clean = 1u << 3;

        // the shapes that recur. `compiling` is the set that turns source into code, and is what replaced
        // the second `for (auto &command : {...})` registration loop this table exists to delete.
        //
        // `test` is one of them and that is the whole of why the row it added is small: every flag about
        // *how* a program is compiled means the same thing for a test run. `jitting` is the narrower pair
        // that runs what it compiled in this process, which is what `--explain prune` is about
        constexpr unsigned int compiling = run | build | test;
        constexpr unsigned int jitting = run | test;
        constexpr unsigned int all = run | build | test | clean;
    };

    unsigned int bit_of(Subcommand id);

    // **every option echoc accepts, as an id.** reading one is `Opt::t_optimize` and never the string
    // `"--optimize"`, which is the whole point of the type: forty call sites spelling a flag name
    // back to a parser would make a rename a value that silently defaulted rather than a compile error
    enum class Opt
    {
        t_output,
        t_module,
        t_target,
        t_filter,
        t_build_dir,
        t_package_dir,
        t_link,
        t_debug,
        t_release,
        t_optimize,
        t_debug_symbols,
        t_no_tbaa,
        t_track_allocations,
        t_no_stdlib,
        t_emit_stdlib_header,
        t_target_os,
        t_target_arch,
        t_define,
        t_target_cpu,
        t_target_features,
        t_print,
        t_explain,
        t_diagnostics,
        t_color,
        t_silent,
        t_verbose,
        t_with_stdlib,
        t_dry_run,
        t_help,
        t_version
    };

    // what `-p, --print <what>` names. **the enumerator order is the table's row order**, and
    // `OptionValue::code` is what ties the two together - see the note there
    enum class PrintKind
    {
        t_ast,
        t_ast_resolved,
        t_ir,
        t_ir_units,
        t_symbols,
        t_instances,
        t_manifest
    };

    // what `--explain <what>` names. **`t_prune` is legal on `run` alone**, because only the JIT prunes -
    // and that is a property of the *value*, not of the option, which is why OptionValue carries a mask
    enum class ExplainKind
    {
        t_cache,
        t_prune,
        t_memory,
        t_time
    };

    // how hard, and over what. three answers rather than two flags that are not each other's
    // inverse: `-O` said what is compiled *together* and `--no-optimize` said whether the per-unit
    // pipeline ran, so `-O --no-optimize` was expressible and meant almost nothing.
    //
    // deliberately **not** on Compiler::CompilerOptions. `t_whole` is a fact about how the driver drove
    // the compiler - merge every unit, then O3, and store no object - and not about the program being
    // compiled, so the readers of `no_optimize` would each owe an arm they must never take. The per-unit
    // half of this answer *is* `no_optimize`, and Compiler::resolve_driver_options sets it from here
    enum class OptimizeMode
    {
        t_none,
        t_module,
        t_whole
    };

    // does the option take a word after it, and may it be given more than once
    enum class OptionArity
    {
        t_flag,             // --silent
        t_value,            // --build-dir <dir>; written twice, the last one wins
        t_repeated_value    // -m <manifest>; every one is kept, in the order written
    };

    // the heading a help page files this option under. **the order of this enum is the order of the
    // sections on the page**, so a category added in the middle moves the page and nothing else
    enum class OptionCategory
    {
        t_inputs,
        t_build,
        t_target,
        t_report,
        t_removal,
        t_general
    };

    const char *category_heading(OptionCategory category);

    // options sharing a group refuse one another. **a group and not a `conflicts_with` pair**, because a
    // pair has to be written on both rows and the day a third member arrives it is written three times
    enum class ExclusionGroup
    {
        t_none,
        t_build_mode,   // --debug / --release
        t_stdlib_use    // --no-stdlib / --emit-stdlib-header
    };

    const char *exclusion_group_name(ExclusionGroup group);

    // one accepted word of a valued option.
    //
    // **the subcommand mask is per value, not per option**, which is the whole reason this is a struct
    // rather than a list of strings: `--explain prune` is legal on `run` and a refusal on `build`. A flag
    // a subcommand accepts and silently ignores is worse than one it rejects, and a *value* is no
    // different - `-O` on `build` was exactly that mistake one level up
    struct OptionValue
    {
        const char *name;
        unsigned int subcommands;

        // the enumerator this word denotes, cast to unsigned. Compiler::PrintKind, Compiler::ExplainKind
        // and Compiler::OptimizeMode are cast back by their own accessors, so the word and its meaning
        // sit in one row rather than in a table here and a switch somewhere else
        unsigned int code;

        // the same two texts an option carries, for the same reason: `summary` is the line the page hangs
        // off the tree, `description` is what `--help <option>` prints under the value. It is the only
        // thing that says what `ir-units` is that `ir` is not, so neither is optional
        const char *summary;
        const char *description;
    };

    // an acceptance rule somebody else owns - Compiler::parse_color_choice and parse_diagnostic_format,
    // which build their own "Expected one of: ..." out of their own tables
    typedef bool (*OptionCheck)(const std::string &value, std::string &out_error);

    // **the sole answer to "what is an option".**
    //
    // one declarative row per flag, and the row is the whole of what echoc knows about it: parsing reads
    // it, validation reads it, the usage line reads it, the help page reads it, and a refusal spells the
    // flag back out of it. It replaced p-ranav/argparse, whose two `for (auto &command : {...})`
    // registration loops in main() were the *only* statement of which subcommand accepted what - so the
    // question "does clean take -g" had no answer anything could ask, only a loop somebody had to read.
    //
    // `description` is the long paragraph `--help <option>` prints. a `//` comment beside each
    // registration is the best documentation nobody can see; putting it here is most of what
    // this table is for
    struct CommandLineOption
    {
        Opt id;

        // the long spelling, without dashes
        const char *name;

        // a second long spelling, or nullptr. **one row and not two**, so `stated()` cannot disagree with
        // itself about `--color` and `--colour`
        const char *alias;

        // the single-character spelling, or '\0'. **single character only** - `-ar`, `-syt`, `-pu` and the
        // rest were shorts that could never be clustered and read as long flags with a dash missing
        char shorthand;

        OptionArity arity;
        OptionCategory category;

        // which subcommands accept it at all
        unsigned int subcommands;

        // which subcommands refuse an invocation that omits it. **zero on every row today**: `build`'s
        // `-o` was the one that had it, and the question stopped being answerable from argv alone the
        // moment a manifest could name its own binaries - see that row, and resolve_programs, which
        // refuses it where the manifest is known. Kept because it is the table's answer to "is this
        // option optional" and the usage line reads it, so a future required option is a row and not a
        // hand-written check in a subcommand
        unsigned int required_by;

        ExclusionGroup exclusion;

        // what the value is called in a usage line - `<manifest>`, `<dir>` - or nullptr for a flag. an
        // option with a `values` list generates one instead, `<none|module|whole>`, so the two cannot drift
        const char *metavar;

        // what CommandLine::value() answers when the option was not written. "" for almost everything;
        // `--diagnostics` and `--color` answer "auto"
        const char *fallback;

        // **two texts and not one, because a page and an answer are different questions.** `summary` is
        // the line `--help` prints beside the flag: one line, lowercase, no trailing period, short enough
        // for the summary column - so a whole command fits on a screen and can be scanned.
        //
        // `description` is what `--help <option>` prints. embedded newlines are paragraph breaks.
        // inlining these on the page would make `echoc build --help` two hundred and fifty lines -
        // complete, correct, and nothing anybody reads
        const char *summary;
        const char *description;

        // **exactly one of these two, or neither.**
        //
        // `values` is the closed vocabulary this subsystem owns - --print, --explain, --optimize - and is
        // then the *only* list of them: acceptance, the metavar and the help page all read it.
        //
        // `check` is for a vocabulary somebody else already owns, so listing those values here as well
        // would be the second list this whole type exists to abolish. Their help says them in prose.
        //
        // neither, for free text: -o, --build-dir, --module, --define, --target-*, --link. `--link`
        // deliberately has no checker - Compiler::parse_link_requirement needs a working directory and a
        // TargetFacts, neither of which exists this early, so its refusal stays where the facts are
        std::vector<OptionValue> values;
        OptionCheck check;
    };

    const std::vector<CommandLineOption> &command_line_options();

    // by id. the vector is built so that index == size_t(id), asserted once at construction
    const CommandLineOption &option_for(Opt id);

    // by spelling, honouring `alias`. null when nothing answers
    const CommandLineOption *option_for_long(const std::string &name);
    const CommandLineOption *option_for_short(char shorthand);

    bool option_takes_a_value(const CommandLineOption &option);

    // `-m, --module <manifest>` - the one spelling a heading line, a usage line and every refusal are
    // built from, so a sentence naming a flag cannot name it the way it is not written
    std::string option_spelling(const CommandLineOption &option);

    // `--module` or `-m, --module`, without the metavar - what a refusal names
    std::string option_flag_names(const CommandLineOption &option);

    // the accepted words this subcommand may write, joined - `none|module|whole`. empty when a checker
    // owns acceptance, or when the option is free text
    std::string option_value_list(const CommandLineOption &option, Subcommand subject);

    // the accepted word for `code`, or nullptr. what a settled enumerator is spelled back as
    const OptionValue *option_value_for_code(const CommandLineOption &option, unsigned int code);

    // the three subcommands, as data for the same reason the options are
    struct SubcommandInfo
    {
        Subcommand id;
        const char *name;
        unsigned int bit;

        // one line, for the COMMANDS block on the top-level page
        const char *summary;

        // the paragraph its own page opens with
        const char *description;

        // does it take `.eco` positionals at all. `clean` parses no source and runs no pass, so a filename
        // on its command line is a mistake and is refused rather than ignored
        bool takes_sources;

        // the positional as a usage line spells it, or nullptr
        const char *positional;

        // the short line the page prints beside it, and the paragraphs `--help <sources>` would - the
        // same split every option row makes, for the same reason
        const char *positional_summary;
        const char *positional_description;

        // does a bare `--` mean anything here. **`run` alone**, and the meaning is "everything after this
        // belongs to the program", never POSIX's "end of options" - this CLI has always meant the first.
        // the consequence is that a source file whose name begins with `-` is unreachable, which is a
        // trade nobody has ever paid and which must not be "fixed" without changing what
        // `echoc run p.eco -- a b` does
        bool takes_program_arguments;
    };

    const std::vector<SubcommandInfo> &subcommand_table();
    const SubcommandInfo *subcommand_for_word(const std::string &word);
    const SubcommandInfo &subcommand_info(Subcommand id);

    // **what a spelling that no longer exists is answered with.**
    //
    // the pseudo-shorts `-O -a -ar -syt -pi -pu -ec -em -ep -ta -t`, the six `--print-*` flags, the three
    // `--explain-*` flags, `--no-optimize`, `--debug-info`, `--timings` and `clean --stdlib`. Each answers
    // with the sentence naming what to write instead.
    //
    // **a refusal and never an acceptance**, and that is not a courtesy question: the e2e corpus
    // byte-compares merged stdout+stderr, so a deprecation *warning* would break every case that used the
    // old spelling - which is the same set of cases that has to be edited anyway - and accepting them
    // silently would give back exactly the drift this table removes. So it buys a good sentence on the
    // failure path and nothing else, and it is deletable in one release
    const char *retired_spelling(const std::string &word);
};

#endif
