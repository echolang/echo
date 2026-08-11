#include "Compiler/CommandLineHelp.h"

#include "Compiler/TerminalStyle.h"
#include "eco.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
    // a heading sits two columns in and a value's connector four, so the tree is visibly inside the
    // option it hangs from. Named rather than spelled at each site that writes one
    constexpr unsigned int HEADING_INDENT = 2;
    constexpr unsigned int BRANCH_INDENT = 4;
    constexpr unsigned int BODY_INDENT = 4;

    // how many *columns* a connector occupies. both themes draw three, which is why one constant serves
    // and why a theme may not add a fourth without the column arithmetic being told
    constexpr unsigned int BRANCH_WIDTH = 3;

    // what a stream with no width gets, and the widest a paragraph is ever set. see page_width()
    constexpr unsigned int ASSUMED_WIDTH = 80;
    constexpr unsigned int MAXIMUM_WIDTH = 100;

    // the furthest right a summary may start. past this a spelling takes the line to itself, so one long
    // one - `--diagnostics <auto|pretty|ascii|json>` - cannot push every other summary on the page right
    constexpr unsigned int MAXIMUM_COLUMN = 30;

    std::string spaces(unsigned int count)
    {
        return std::string(count, ' ');
    }

    // the usage line for one subcommand, without the leading indent. built out of the table so a required
    // option shows up here the moment its row says so
    std::string usage_of(Compiler::Subcommand id)
    {
        const Compiler::SubcommandInfo &info = Compiler::subcommand_info(id);

        std::string line = std::string("echoc ") + info.name + " [options]";

        for (const Compiler::CommandLineOption &option : Compiler::command_line_options()) {
            if ((option.required_by & info.bit) == 0) {
                continue;
            }

            line += " -";

            if (option.shorthand != '\0') {
                line += option.shorthand;
            }
            else {
                line += std::string("-") + option.name;
            }

            line += " ";
            line += option.metavar != nullptr ? option.metavar : "<value>";
        }

        if (info.positional != nullptr) {
            line += std::string(" ") + info.positional;
        }

        if (info.takes_program_arguments) {
            line += " [-- <program arguments>]";
        }

        return line;
    }

    // `-p, --print <what>` - what the page prints on the left of a summary. **the metavar is the row's
    // own** and is never generated from the value list here: the values are drawn as a tree underneath,
    // so spelling them in the heading as well would say the same thing twice and push the column right
    std::string page_spelling(const Compiler::CommandLineOption &option)
    {
        std::string spelled = Compiler::option_flag_names(option);

        if (option.metavar != nullptr) {
            spelled += std::string(" ") + option.metavar;
        }

        return spelled;
    }

    // does this subcommand accept this word. `t_none` - the overview - accepts everything, because there
    // is no command yet for a marker to be about
    bool value_is_available(
        const Compiler::OptionValue &value,
        Compiler::Subcommand subject
    )
    {
        if (subject == Compiler::Subcommand::t_none) {
            return true;
        }

        return (value.subcommands & Compiler::bit_of(subject)) != 0;
    }

    // `run only` for a value this page's command cannot write, `default` for the one its option falls
    // back to, empty otherwise. Both read off the table rather than being written per row
    std::string marker_for(
        const Compiler::CommandLineOption &option,
        const Compiler::OptionValue &value,
        Compiler::Subcommand subject
    )
    {
        if (!value_is_available(value, subject)) {
            // the commands that *can* answer it, named - so the marker is the remedy rather than only a
            // refusal, and it stays right when a value is opened up to a second command
            std::string commands;

            for (const Compiler::SubcommandInfo &info : Compiler::subcommand_table()) {
                if ((value.subcommands & info.bit) == 0) {
                    continue;
                }

                if (!commands.empty()) {
                    commands += "/";
                }

                commands += info.name;
            }

            return commands + " only";
        }

        if (option.fallback != nullptr && value.name == std::string(option.fallback)) {
            return "default";
        }

        return "";
    }
};

Compiler::HelpTheme Compiler::HelpTheme::pretty()
{
    return HelpTheme { "├─ ", "└─ ", "▌", "─" };
}

Compiler::HelpTheme Compiler::HelpTheme::ascii()
{
    return HelpTheme { "|- ", "`- ", "", "" };
}

Compiler::CommandLineHelp::CommandLineHelp(std::ostream &out, TerminalCapabilities capabilities) :
    _out(out),
    _capabilities(capabilities),
    _theme(capabilities.unicode ? HelpTheme::pretty() : HelpTheme::ascii())
{}

std::string Compiler::CommandLineHelp::styled(const std::string &text, const char *sgr) const
{
    return Compiler::styled(text, sgr, _capabilities.color);
}

unsigned int Compiler::CommandLineHelp::page_width() const
{
    if (_capabilities.width == 0) {
        return ASSUMED_WIDTH;
    }

    return std::min(_capabilities.width, MAXIMUM_WIDTH);
}

unsigned int Compiler::CommandLineHelp::summary_column(Subcommand subject) const
{
    const unsigned int wanted = subject == Subcommand::t_none ? ~0u : bit_of(subject);

    // measured as *drawn*, indent included, so the answer is the column a row actually needs rather than
    // a spelling length something later has to add an indent back onto
    size_t widest = 0;

    if (subject == Subcommand::t_none) {
        for (const SubcommandInfo &info : subcommand_table()) {
            widest = std::max(widest, HEADING_INDENT + std::strlen(info.name));
        }
    }
    else if (subcommand_info(subject).positional != nullptr) {
        widest = HEADING_INDENT + std::strlen(subcommand_info(subject).positional);
    }

    for (const CommandLineOption &option : command_line_options()) {
        if ((option.subcommands & wanted) == 0) {
            continue;
        }

        widest = std::max(widest, HEADING_INDENT + page_spelling(option).size());

        for (const OptionValue &value : option.values) {
            widest = std::max(widest, BRANCH_INDENT + BRANCH_WIDTH + std::strlen(value.name));
        }
    }

    // two spaces of gutter after the widest row, then the cap - and never past half the page, or a
    // narrow terminal spends its whole width on the gutter and leaves the summaries nowhere to go
    return std::min(
        std::min(static_cast<unsigned int>(widest) + 2, MAXIMUM_COLUMN), page_width() / 2);
}

void Compiler::CommandLineHelp::write_wrapped(const std::string &text, unsigned int indent) const
{
    const std::string margin = spaces(indent);

    // **a newline in a description is a paragraph break, and is drawn as one.** Each embedded line is
    // wrapped on its own with a blank line between, rather than the whole string being reflowed into one
    // block - which is what lets a description carry a short indented list of its values
    size_t line_start = 0;
    bool first = true;
    bool previous_was_indented = false;

    while (line_start <= text.size()) {
        const size_t line_end = std::min(text.find('\n', line_start), text.size());
        const std::string line = text.substr(line_start, line_end - line_start);
        const bool indented = !line.empty() && line.front() == ' ';

        // **two indented lines in a row are one block, not two paragraphs.** A blank line between them is
        // what turns a two-command example into two examples that look unrelated - and a run of commands
        // is the whole reason a description may indent a line at all
        if (!first && !(indented && previous_was_indented)) {
            _out << std::endl;
        }

        first = false;
        previous_was_indented = indented;

        // a line the author indented keeps that indent on every row it wraps to, so a list inside a
        // paragraph stays a list rather than unravelling at the first wrap
        const size_t written = line.find_first_not_of(' ');
        const unsigned int hang = written == std::string::npos
            ? 0
            : static_cast<unsigned int>(written);

        size_t at = 0;

        while (at < line.size()) {
            const unsigned int margin_width = indent + (at == 0 ? 0 : hang);

            // the width left for words, never less than eight so a very narrow terminal makes progress
            // rather than emitting an empty line per word
            const unsigned int usable = page_width() > margin_width + 8
                ? page_width() - margin_width
                : 8;

            size_t take = line.size() - at;

            if (take > usable) {
                const size_t space = line.rfind(' ', at + usable);

                take = space != std::string::npos && space > at
                    ? space - at
                    : usable;
            }

            _out << margin << spaces(at == 0 ? 0 : hang) << line.substr(at, take) << std::endl;

            at += take;

            // exactly one break is eaten; the next row starts on a word
            while (at < line.size() && line[at] == ' ') {
                at++;
            }
        }

        if (line_end == text.size()) {
            break;
        }

        line_start = line_end + 1;
    }
}

void Compiler::CommandLineHelp::write_row(
    const std::string &spelling,
    unsigned int spelling_width,
    const std::string &summary,
    const std::string &marker,
    unsigned int column
) const
{
    // **the width is a parameter and is never measured off the string.** By the time a row is drawn its
    // spelling may carry SGR escapes and box-drawing bytes, so `.size()` answers neither columns nor
    // characters - the column would drift under `--color=always` and again under unicode
    size_t visible = spelling_width;

    std::string line = spelling;

    // **a spelling past the column takes the line to itself.** The alternative is pushing every summary
    // on the page right to accommodate one long one, which is how the column stops being readable
    if (visible + 1 > column) {
        _out << line << std::endl;
        line = "";
        visible = 0;
    }

    line += spaces(static_cast<unsigned int>(column - visible));
    visible = column;

    // **the summary wraps at the column too**, so a long one continues under itself rather than off the
    // edge - the column is the whole point of the page, and a row that escapes it is a row that breaks it
    const std::string marked = marker.empty() ? "" : "(" + marker + ")";

    // never less than eight, and computed so it cannot underflow: `column` is bounded by half the page,
    // but a marker wide enough to eat the rest is still expressible
    const unsigned int room = page_width() > column + 8 ? page_width() - column : 8;

    // the marker is right-aligned on the first row **only where the summary still fits beside it**.
    // Where it does not, it is appended to the text and wrapped like any other word - which is what a
    // forty-column terminal gets, rather than a row hanging off the edge
    const unsigned int reserved = marked.empty()
        ? 0
        : static_cast<unsigned int>(marked.size()) + 2;

    const bool align_marker = !marked.empty()
        && room > reserved
        && summary.size() <= room - reserved;

    std::string rest = align_marker || marked.empty()
        ? summary
        : summary + " " + marked;

    bool first = true;

    while (true) {
        size_t take = rest.size();

        if (take > room) {
            const size_t space = rest.rfind(' ', room);
            take = space != std::string::npos && space > 0 ? space : room;
        }

        line += rest.substr(0, take);
        visible += take;

        if (first && align_marker) {
            line += spaces(static_cast<unsigned int>(page_width() - visible - marked.size()));
            line += styled(marked, Compiler::sgr::dim);
        }

        _out << line << std::endl;

        rest.erase(0, take);

        while (!rest.empty() && rest.front() == ' ') {
            rest.erase(0, 1);
        }

        if (rest.empty()) {
            break;
        }

        line = spaces(column);
        visible = column;
        first = false;
    }
}

// **a heading is drawn, not shouted.** It used to be upper-cased, which is the one decoration a terminal
// cannot decline - a reader on a console with no colour got SHOUTING and a reader with colour got shouting
// as well. Bold plus a mark and a rule is the same emphasis asked of the terminal instead, and it costs
// nothing where the terminal says no: the ascii theme draws neither glyph, leaving the words
void Compiler::CommandLineHelp::render_heading(const std::string &text) const
{
    std::string line;
    unsigned int visible = 0;

    if (_theme.heading_mark[0] != '\0') {
        line += styled(_theme.heading_mark, Compiler::sgr::secondary) + " ";
        visible += 2;
    }

    line += styled(text, Compiler::sgr::bold);
    visible += static_cast<unsigned int>(text.size());

    // the rule runs to the page width, which is what makes a section read as a band rather than as a
    // word. Dim, because it is furniture: it must never compete with the option names under it
    // one column short of the page, because a line filling it exactly wraps on a terminal of that width
    // and the newline after it then lands a blank row in the middle of the page
    if (_theme.heading_rule[0] != '\0' && visible + 3 < page_width()) {
        std::string rule = " ";

        for (unsigned int i = visible + 1; i < page_width() - 1; i++) {
            rule += _theme.heading_rule;
        }

        line += styled(rule, Compiler::sgr::dim);
    }

    _out << line << std::endl;
}

void Compiler::CommandLineHelp::render_version() const
{
    _out << ECO_VERSION_STRING << std::endl;
}

void Compiler::CommandLineHelp::render_usage(Subcommand subject) const
{
    render_heading("Usage");

    if (subject != Subcommand::t_none) {
        _out << spaces(HEADING_INDENT) << usage_of(subject) << std::endl;
        return;
    }

    _out << spaces(HEADING_INDENT) << "echoc <command> [options] [sources...]" << std::endl;
    _out << std::endl;

    for (const SubcommandInfo &info : subcommand_table()) {
        _out << spaces(HEADING_INDENT) << usage_of(info.id) << std::endl;
    }
}

void Compiler::CommandLineHelp::render_error(const std::string &message, Subcommand subject) const
{
    _out << styled("error", Compiler::sgr::error) << ": " << message << std::endl;
    _out << std::endl;

    render_usage(subject);

    _out << std::endl;

    if (subject == Subcommand::t_none) {
        write_wrapped("Run 'echoc --help' to see what echoc can do.", 0);
        return;
    }

    write_wrapped(
        std::string("Run 'echoc ") + subcommand_info(subject).name
            + " --help' for what this command accepts.",
        0);
}

void Compiler::CommandLineHelp::render_option(
    const CommandLineOption &option,
    Subcommand subject,
    unsigned int column
) const
{
    write_row(
        spaces(HEADING_INDENT) + styled(page_spelling(option), Compiler::sgr::info),
        HEADING_INDENT + static_cast<unsigned int>(page_spelling(option).size()),
        option.summary,
        "",
        column);

    // **every value, marked rather than filtered.** A reader who cannot see `prune` on the build page
    // cannot learn it exists; writing it there is still the refusal that names `run`
    for (size_t i = 0; i < option.values.size(); i++) {
        const OptionValue &value = option.values[i];
        const bool last = i + 1 == option.values.size();

        write_row(
            spaces(BRANCH_INDENT) + styled(last ? _theme.last_branch : _theme.branch,
                Compiler::sgr::dim) + styled(value.name, Compiler::sgr::secondary),
            BRANCH_INDENT + BRANCH_WIDTH + static_cast<unsigned int>(std::strlen(value.name)),
            value.summary,
            marker_for(option, value, subject),
            column);
    }
}

void Compiler::CommandLineHelp::render_sources(
    const SubcommandInfo &info,
    unsigned int column
) const
{
    write_row(
        spaces(HEADING_INDENT) + styled(info.positional, Compiler::sgr::info),
        HEADING_INDENT + static_cast<unsigned int>(std::strlen(info.positional)),
        info.positional_summary,
        "",
        column);
}

void Compiler::CommandLineHelp::render_category(
    Subcommand subject,
    OptionCategory category,
    unsigned int column
) const
{
    const unsigned int wanted = subject == Subcommand::t_none ? ~0u : bit_of(subject);

    // **the sources entry is part of this category and is drawn by hand**, because a positional has no
    // row in the option table: it has no spelling to look up, no arity and no exclusion, and giving it a
    // row would mean every reader of that table owing an arm for the one entry that is not an option
    const bool has_sources = category == OptionCategory::t_inputs
        && subject != Subcommand::t_none
        && subcommand_info(subject).positional != nullptr;

    bool drawn = false;

    if (has_sources) {
        _out << std::endl;
        render_heading(category_heading(category));

        render_sources(subcommand_info(subject), column);
        drawn = true;
    }

    for (const CommandLineOption &option : command_line_options()) {
        if (option.category != category || (option.subcommands & wanted) == 0) {
            continue;
        }

        if (!drawn) {
            _out << std::endl;
            render_heading(category_heading(category));
            drawn = true;
        }

        render_option(option, subject, column);
    }

    // a category with nothing in it is absent rather than an empty heading, which is what keeps
    // WHAT CLEAN REMOVES off the build page and the whole page honest about what this command takes
}

void Compiler::CommandLineHelp::render_overview() const
{
    write_wrapped(
        std::string("echoc ") + ECO_VERSION_STRING + " - the echo programming language compiler.", 0);

    _out << std::endl;

    render_usage(Subcommand::t_none);

    _out << std::endl;
    render_heading("Commands");

    const unsigned int column = summary_column(Subcommand::t_none);

    for (const SubcommandInfo &info : subcommand_table()) {
        write_row(
            spaces(HEADING_INDENT) + styled(info.name, Compiler::sgr::info),
            HEADING_INDENT + static_cast<unsigned int>(std::strlen(info.name)),
            info.summary,
            "",
            column);
    }

    render_category(Subcommand::t_none, OptionCategory::t_general, column);

    _out << std::endl;
    write_wrapped("Run 'echoc <command> --help' for what a command accepts.", 0);
}

void Compiler::CommandLineHelp::render_help(Subcommand subject) const
{
    if (subject == Subcommand::t_none) {
        render_overview();
        return;
    }

    const SubcommandInfo &info = subcommand_info(subject);
    const unsigned int column = summary_column(subject);

    write_wrapped(std::string("echoc ") + info.name + " - " + info.summary + ".", 0);

    _out << std::endl;

    render_usage(subject);

    // **the command's own prose, and its positional's, belong on its page.** Both were written into the
    // table and neither had a reader, which made them a description nobody could reach - the one thing
    // `--help <option>` exists to prevent for the options. A positional has no spelling to name as a
    // help topic, so this page is the only place its paragraphs can go
    _out << std::endl;
    write_wrapped(info.description, 0);

    if (info.positional != nullptr) {
        _out << std::endl;
        _out << spaces(HEADING_INDENT) << styled(info.positional, Compiler::sgr::info) << std::endl;

        write_wrapped(info.positional_description, BODY_INDENT);
    }

    for (const OptionCategory category : {
             OptionCategory::t_inputs,
             OptionCategory::t_build,
             OptionCategory::t_target,
             OptionCategory::t_report,
             OptionCategory::t_removal,
             OptionCategory::t_general }) {
        render_category(subject, category, column);
    }

    _out << std::endl;

    // **the prose is one command away, and the page has to say so** - a description nobody can find is a
    // description nobody wrote
    write_wrapped(
        std::string("Run 'echoc ") + info.name
            + " --help <option>' for the full description of one option, for example 'echoc "
            + info.name + " --help optimize'.",
        0);
}

void Compiler::CommandLineHelp::render_option_help(
    Subcommand subject,
    const CommandLineOption &option
) const
{
    render_heading(option_spelling(option));

    _out << std::endl;

    write_wrapped(option.description, BODY_INDENT);

    // **the values are rendered from the table, not spelled into the prose.** They are structured data
    // already; a list written into the description would be a second copy of the words, the masks and the
    // default, and the copy is the one that goes stale
    for (const OptionValue &value : option.values) {
        _out << std::endl;
        _out << spaces(BODY_INDENT) << styled(value.name, Compiler::sgr::secondary);

        const std::string marker = marker_for(option, value, subject);

        if (!marker.empty()) {
            _out << styled("  (" + marker + ")", Compiler::sgr::dim);
        }

        _out << std::endl;

        write_wrapped(value.description, BODY_INDENT + 2);
    }

    // the facts the prose should not have to repeat, because they are on the row and would go stale in a
    // sentence: which commands take it, whether it may be repeated, and what it answers when unwritten
    std::string commands;

    for (const SubcommandInfo &info : subcommand_table()) {
        if ((option.subcommands & info.bit) == 0) {
            continue;
        }

        if (!commands.empty()) {
            commands += ", ";
        }

        commands += info.name;
    }

    _out << std::endl;
    write_wrapped("Accepted by: " + commands + ".", BODY_INDENT);

    if (option.arity == OptionArity::t_repeated_value) {
        write_wrapped("May be given more than once; every one is kept.", BODY_INDENT);
    }

    if (option.fallback != nullptr && option.fallback[0] != '\0') {
        write_wrapped(std::string("Defaults to '") + option.fallback + "'.", BODY_INDENT);
    }

    if ((option.required_by & bit_of(subject)) != 0) {
        write_wrapped(
            std::string("Required by '") + subcommand_info(subject).name + "'.", BODY_INDENT);
    }
}
