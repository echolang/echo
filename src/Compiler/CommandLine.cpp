#include "Compiler/CommandLine.h"

#include <fmt/core.h>

#include <cstddef>

namespace
{
    // the value words of an option whose vocabulary this subsystem owns, joined for a refusal. asked of
    // the *subcommand* rather than of the option, so `build --explain prune` is told what build accepts
    std::string accepted_here(const Compiler::CommandLineOption &option, Compiler::Subcommand subject)
    {
        return Compiler::option_value_list(option, subject);
    }

    // `run`, or `echoc` when no subcommand has been read yet. what a refusal calls the thing refusing
    std::string subject_name(Compiler::Subcommand subject)
    {
        if (subject == Compiler::Subcommand::t_none) {
            return "echoc";
        }

        return Compiler::subcommand_info(subject).name;
    }

    // every command echoc has, joined the way a refusal quotes them back: `'run', 'build' or 'clean'`.
    //
    // **read off the table rather than spelled here**, which is what the table is for - this was the one
    // sentence in the command line that knew the list by heart, and adding a command left it lying
    std::string command_word_list()
    {
        const std::vector<Compiler::SubcommandInfo> &table = Compiler::subcommand_table();
        std::string joined;

        for (size_t index = 0; index < table.size(); index++) {
            if (index > 0) {
                joined += index + 1 == table.size() ? " or " : ", ";
            }

            joined += fmt::format("'{}'", table[index].name);
        }

        return joined;
    }

    // **a word that begins with two dashes is never a value.** `-` alone is, so `--target-features -crc`
    // works, and so does a relative path. Without this, `echoc build -o --silent` writes a file called
    // --silent and the missing output is reported by clang
    bool looks_like_a_flag(const std::string &word)
    {
        return word.size() > 1 && word[0] == '-' && word[1] == '-';
    }
};

const Compiler::CommandLine::Given &Compiler::CommandLine::given(Opt id) const
{
    static const Given nothing;

    const size_t index = static_cast<size_t>(id);
    if (index >= _given.size()) {
        return nothing;
    }

    return _given[index];
}

bool Compiler::CommandLine::stated(Opt id) const
{
    return given(id).stated;
}

bool Compiler::CommandLine::flag(Opt id) const
{
    return given(id).stated;
}

const std::string &Compiler::CommandLine::value(Opt id) const
{
    const Given &held = given(id);

    if (held.words.empty()) {
        // the row's own fallback, materialised once so a reference to it outlives this call
        static const std::vector<std::string> fallbacks = [] {
            std::vector<std::string> written;

            for (const CommandLineOption &option : command_line_options()) {
                written.push_back(option.fallback);
            }

            return written;
        }();

        return fallbacks[static_cast<size_t>(id)];
    }

    return held.words.back();
}

const std::vector<std::string> &Compiler::CommandLine::list(Opt id) const
{
    return given(id).words;
}

bool Compiler::CommandLine::holds_value(Opt id, const char *word) const
{
    for (const std::string &written : given(id).words) {
        if (written == word) {
            return true;
        }
    }

    return false;
}

bool Compiler::CommandLine::prints(PrintKind what) const
{
    const OptionValue *value = option_value_for_code(
        option_for(Opt::t_print), static_cast<unsigned int>(what));

    return value != nullptr && holds_value(Opt::t_print, value->name);
}

bool Compiler::CommandLine::explains(ExplainKind what) const
{
    const OptionValue *value = option_value_for_code(
        option_for(Opt::t_explain), static_cast<unsigned int>(what));

    return value != nullptr && holds_value(Opt::t_explain, value->name);
}

Compiler::OptimizeMode Compiler::CommandLine::optimize() const
{
    const CommandLineOption &option = option_for(Opt::t_optimize);
    const std::string &written = value(Opt::t_optimize);

    for (const OptionValue &accepted : option.values) {
        if (written == accepted.name) {
            return static_cast<OptimizeMode>(accepted.code);
        }
    }

    return OptimizeMode::t_module;
}

Compiler::ColorChoice Compiler::CommandLine::color_choice() const
{
    ColorChoice choice = ColorChoice::t_auto;
    std::string ignored;

    parse_color_choice(value(Opt::t_color), choice, ignored);

    return choice;
}

Compiler::DiagnosticFormat Compiler::CommandLine::diagnostic_format() const
{
    DiagnosticFormat format = DiagnosticFormat::t_auto;
    std::string ignored;

    parse_diagnostic_format(value(Opt::t_diagnostics), format, ignored);

    return format;
}

namespace
{
    // one accepted word, recorded. **the acceptance rules live here and only here** - which subcommand
    // may write the option, which subcommand may write this particular value, and whatever checker the
    // row names
    bool accept_value(
        const Compiler::CommandLineOption &option,
        const std::string &word,
        Compiler::Subcommand subject,
        std::string &out_error
    )
    {
        if (option.check != nullptr) {
            return option.check(word, out_error);
        }

        if (option.values.empty()) {
            return true;
        }

        for (const Compiler::OptionValue &accepted : option.values) {
            if (word != accepted.name) {
                continue;
            }

            // the value exists but this command cannot answer it - `--explain prune` on `build`. named
            // separately from an unknown word, because the two are different mistakes and the remedy for
            // this one is a different subcommand rather than a different spelling
            if (subject != Compiler::Subcommand::t_none
                && (accepted.subcommands & Compiler::bit_of(subject)) == 0) {
                out_error = fmt::format(
                    "'{} --{} {}' is not something '{}' can answer. It accepts: {}.",
                    subject_name(subject),
                    option.name,
                    word,
                    subject_name(subject),
                    accepted_here(option, subject)
                );

                return false;
            }

            return true;
        }

        out_error = fmt::format(
            "Unknown '--{}' value '{}'. Expected one of: {}.",
            option.name,
            word,
            accepted_here(option, subject)
        );

        return false;
    }
};

bool Compiler::parse_command_line(
    int argc,
    const char *const *argv,
    CommandLine &out,
    std::string &out_error
)
{
    // **every field, not only the values.** Resetting `_given` alone left `subcommand`, `sources` and the
    // two answers carrying whatever a previous parse into the same object had put there - which the
    // driver never sees, since it parses once into a fresh one, and which is exactly why it would have
    // survived to bite the first caller that reused one
    out = CommandLine();
    out._given.assign(command_line_options().size(), CommandLine::Given());

    // **everything after the first bare `--` belongs to the program, not to echoc.** Split before the
    // loop rather than inside it: sources are recognised at any position, so a tail left in the stream
    // would be read as filenames and `echoc run p.eco -- a b` would look for a file called 'a'
    std::vector<std::string> words;
    bool in_program_arguments = false;

    for (int i = 1; i < argc; i++) {
        const std::string word = argv[i];

        if (in_program_arguments) {
            out.program_arguments.push_back(word);
            continue;
        }

        if (word == "--") {
            in_program_arguments = true;
            continue;
        }

        words.push_back(word);
    }

    for (size_t i = 0; i < words.size(); i++) {
        const std::string &word = words[i];

        const CommandLineOption *option = nullptr;
        std::string spelled;
        std::string attached;
        bool has_attached = false;

        if (word.size() > 2 && word[0] == '-' && word[1] == '-') {
            const size_t equals = word.find('=');

            spelled = equals == std::string::npos
                ? word.substr(2)
                : word.substr(2, equals - 2);

            if (equals != std::string::npos) {
                attached = word.substr(equals + 1);
                has_attached = true;
            }

            option = option_for_long(spelled);
            spelled = "--" + spelled;
        }
        else if (word.size() > 1 && word[0] == '-') {
            spelled = word;

            if (word.size() == 2) {
                option = option_for_short(word[1]);
            }
        }
        else {
            // a positional. the first one names the subcommand, every one after it is a source
            if (out.subcommand == Subcommand::t_none) {
                const SubcommandInfo *info = subcommand_for_word(word);

                if (info == nullptr) {
                    out_error = fmt::format(
                        "'{}' is not an echoc command. Write {}.", word, command_word_list());

                    return false;
                }

                out.subcommand = info->id;
                continue;
            }

            out.sources.push_back(word);
            continue;
        }

        if (option == nullptr) {
            // a spelling this overhaul removed gets the sentence naming its replacement, ahead of the
            // generic refusal - which would otherwise say '-ar' is unknown and leave the reader guessing
            const char *retired = retired_spelling(word);
            if (retired == nullptr && has_attached) {
                retired = retired_spelling(spelled);
            }

            if (retired != nullptr) {
                out_error = retired;
                return false;
            }

            // a multi-character `-xyz` is neither a cluster nor a long flag here, and saying so beats
            // "unknown option" - it is the mistake somebody coming from the old spellings will make
            if (word[1] != '-' && word.size() > 2) {
                out_error = fmt::format(
                    "'{}' is not an option. A short option is one character - write '-{}' - and a long "
                    "one takes two dashes.",
                    word,
                    word.substr(1)
                );

                return false;
            }

            out_error = fmt::format("Unknown option '{}'.", spelled);

            return false;
        }

        if (out.subcommand != Subcommand::t_none
            && (option->subcommands & bit_of(out.subcommand)) == 0) {
            out_error = fmt::format(
                "'{}' does not take '{}'.",
                subject_name(out.subcommand),
                option_flag_names(*option)
            );

            return false;
        }

        CommandLine::Given &held = out._given[static_cast<size_t>(option->id)];

        if (!option_takes_a_value(*option)) {
            if (has_attached) {
                out_error = fmt::format(
                    "'--{}' takes no value.", option->name);

                return false;
            }

            held.stated = true;
            continue;
        }

        std::string value;

        if (has_attached) {
            value = attached;
        }
        else {
            if (i + 1 >= words.size()) {
                out_error = fmt::format(
                    "'{}' needs a value.", option_spelling(*option));

                return false;
            }

            if (looks_like_a_flag(words[i + 1])) {
                out_error = fmt::format(
                    "'{}' needs a value, and '{}' is an option.",
                    option_flag_names(*option),
                    words[i + 1]
                );

                return false;
            }

            value = words[i + 1];
            i++;
        }

        if (!accept_value(*option, value, out.subcommand, out_error)) {
            return false;
        }

        held.stated = true;

        // a repeated option keeps every word; a plain valued one is answered by the last, and value()
        // reads the back of this list either way
        if (option->arity == OptionArity::t_repeated_value) {
            held.words.push_back(value);
        }
        else {
            held.words.assign(1, value);
        }
    }

    out.wants_help = out.flag(Opt::t_help);
    out.wants_version = out.flag(Opt::t_version);

    // **the two answers short-circuit every remaining rule.** `echoc build --help` must print the page
    // rather than complain about a missing -o, or the page is unreachable from exactly the state that
    // needs it
    if (out.wants_help || out.wants_version) {
        if (out.wants_help && out.sources.size() == 1) {
            const std::string &topic = out.sources.front();

            // **the bare name, because a written `--optimize` never gets here.** A word spelled with its
            // dashes is read as the option itself further up this loop - and one that takes a value would
            // swallow whatever came next - so `--help optimize` is the spelling, and a lone character
            // answers the short form for the same reason
            const CommandLineOption *named = topic.size() == 1
                ? option_for_short(topic[0])
                : option_for_long(topic);

            if (named != nullptr && out.subcommand != Subcommand::t_none
                && (named->subcommands & bit_of(out.subcommand)) == 0) {
                out_error = fmt::format(
                    "'{}' does not take '{}'.",
                    subject_name(out.subcommand),
                    option_flag_names(*named)
                );

                return false;
            }

            out.help_topic = named;
        }

        return true;
    }

    if (out.subcommand == Subcommand::t_none) {
        out_error = "No command given.";
        return false;
    }

    const SubcommandInfo &info = subcommand_info(out.subcommand);
    const unsigned int bit = bit_of(out.subcommand);

    if (!info.takes_sources && !out.sources.empty()) {
        out_error = fmt::format(
            "'{}' takes no source files. It parses none and runs no pass.", info.name);

        return false;
    }

    if (!info.takes_program_arguments && !out.program_arguments.empty()) {
        out_error = fmt::format(
            "Only 'run' passes arguments to the program, so '--' means nothing to '{}'.", info.name);

        return false;
    }

    // the exclusions and the requirements, both read off the rows rather than checked by hand where they
    // happen to matter - so the usage line and the refusal cannot disagree about either
    for (const CommandLineOption &option : command_line_options()) {
        if ((option.required_by & bit) != 0 && !out.stated(option.id)) {
            out_error = fmt::format(
                "'{}' needs '{}'.", info.name, option_spelling(option));

            return false;
        }

        if (option.exclusion == ExclusionGroup::t_none || !out.stated(option.id)) {
            continue;
        }

        for (const CommandLineOption &other : command_line_options()) {
            if (other.id == option.id || other.exclusion != option.exclusion) {
                continue;
            }

            if (out.stated(other.id)) {
                out_error = fmt::format(
                    "'--{}' and '--{}' are two answers to one question - the {}. Write one.",
                    option.name,
                    other.name,
                    exclusion_group_name(option.exclusion)
                );

                return false;
            }
        }
    }

    return true;
}
