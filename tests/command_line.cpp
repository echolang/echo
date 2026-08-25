#include <catch2/catch_test_macros.hpp>

#include <Compiler/CommandLine.h>
#include <Compiler/CommandLineHelp.h>
#include <Compiler/CommandLineOption.h>
#include <Compiler/DriverOptions.h>
#include <Compiler/TerminalCapabilities.h>
#include <eco.h>

#include <cctype>
#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "subprocess.h"
#include "terminal_fixture.h"

// what the e2e corpus cannot assert about the command line, and it is most of it: the corpus only ever
// writes invocations that work, so every refusal, every retired spelling and the whole of the help page
// are unreachable from there.
//
// three levels, in the order the driver runs them - the table's own invariants, then the parse, then the
// resolution - plus the help page, asserted structurally rather than as a golden. See the note above the
// help section for why that is deliberate

using Compiler::CommandLine;
using Compiler::CommandLineHelp;
using Compiler::CommandLineOption;
using Compiler::DriverOptions;
using Compiler::ExplainKind;
using Compiler::Opt;
using Compiler::OptimizeMode;
using Compiler::OptionArity;
using Compiler::OptionCategory;
using Compiler::PrintKind;
using Compiler::Subcommand;
using Compiler::TerminalCapabilities;
using EchoTests::a_terminal;

namespace
{
    // an argv, the way the process gets one. argv[0] is never read and is here so the offsets match
    bool parse(const std::vector<const char *> &words, CommandLine &out, std::string &error)
    {
        std::vector<const char *> argv = { "echoc" };
        argv.insert(argv.end(), words.begin(), words.end());

        return Compiler::parse_command_line(
            static_cast<int>(argv.size()), argv.data(), out, error);
    }

    // the sentence a refusal produced, for a case that is expected to fail
    std::string refusal(const std::vector<const char *> &words)
    {
        CommandLine cli;
        std::string error;

        if (parse(words, cli, error)) {
            return "<accepted>";
        }

        return error;
    }

    // the settled options of an invocation that is expected to be accepted
    DriverOptions resolved(const std::vector<const char *> &words)
    {
        CommandLine cli;
        std::string error;

        REQUIRE(parse(words, cli, error));

        DriverOptions driver;
        REQUIRE(Compiler::resolve_driver_options(cli, driver, error));

        return driver;
    }

    std::string page(Subcommand subject, TerminalCapabilities capabilities)
    {
        std::ostringstream out;
        CommandLineHelp(out, capabilities).render_help(subject);

        return out.str();
    }

    // every SGR sequence removed, so a coloured page can be compared against a plain one
    std::string without_escapes(const std::string &text)
    {
        std::string stripped;

        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] != '\x1b') {
                stripped += text[i];
                continue;
            }

            while (i < text.size() && text[i] != 'm') {
                i++;
            }
        }

        return stripped;
    }

    // **columns, not bytes.** A heading's rule is drawn with box-drawing glyphs, three bytes each, so
    // `.size()` answers neither columns nor characters - a page that fits perfectly would fail a byte
    // check and a page that overflows could pass one
    size_t display_width(const std::string &text)
    {
        size_t columns = 0;

        for (const unsigned char byte : text) {
            if ((byte & 0xc0) != 0x80) {
                columns++;
            }
        }

        return columns;
    }

    std::vector<std::string> lines_of(const std::string &text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;

        while (std::getline(stream, line)) {
            lines.push_back(line);
        }

        return lines;
    }

    bool contains(const std::string &haystack, const std::string &needle)
    {
        return haystack.find(needle) != std::string::npos;
    }
};

TEST_CASE("the option table is indexed by its own ids", "[cli]")
{
    const std::vector<CommandLineOption> &table = Compiler::command_line_options();

    REQUIRE(table.size() == static_cast<size_t>(Opt::t_version) + 1);

    for (size_t i = 0; i < table.size(); i++) {
        REQUIRE(static_cast<size_t>(table[i].id) == i);
    }
}

TEST_CASE("every spelling in the option table is unique", "[cli]")
{
    std::set<std::string> longs;
    std::set<char> shorts;

    for (const CommandLineOption &option : Compiler::command_line_options()) {
        REQUIRE(longs.insert(option.name).second);

        if (option.alias != nullptr) {
            REQUIRE(longs.insert(option.alias).second);
        }

        if (option.shorthand != '\0') {
            REQUIRE(shorts.insert(option.shorthand).second);
        }
    }
}

// **a hard list, so an eighth short option is a decision somebody makes in this file.** The spellings this
// overhaul removed - `-ar`, `-syt`, `-pu`, `-ec` and the rest - were shorts that could never be clustered
// and read as long flags with a dash missing
TEST_CASE("only single-character shorthands exist, and only these seven", "[cli]")
{
    const std::string allowed = "hvompgn";

    for (const CommandLineOption &option : Compiler::command_line_options()) {
        if (option.shorthand == '\0') {
            continue;
        }

        INFO("--" << option.name);
        REQUIRE(allowed.find(option.shorthand) != std::string::npos);
    }
}

TEST_CASE("a flag carries no value machinery and a valued one carries exactly one rule", "[cli]")
{
    // free text: a value nothing in this subsystem is entitled to judge. an explicit list, so adding a
    // valued option with no acceptance rule fails here rather than accepting anything forever
    const std::set<Opt> free_text = {
        Opt::t_output, Opt::t_module, Opt::t_target, Opt::t_build_dir, Opt::t_package_dir, Opt::t_link, Opt::t_define,
        Opt::t_target_os, Opt::t_target_arch, Opt::t_target_cpu, Opt::t_target_features
    };

    for (const CommandLineOption &option : Compiler::command_line_options()) {
        INFO("--" << option.name);

        if (option.arity == OptionArity::t_flag) {
            REQUIRE(option.metavar == nullptr);
            REQUIRE(option.values.empty());
            REQUIRE(option.check == nullptr);
            continue;
        }

        REQUIRE_FALSE((!option.values.empty() && option.check != nullptr));

        if (option.values.empty() && option.check == nullptr) {
            REQUIRE(free_text.count(option.id) == 1);
        }
    }
}

TEST_CASE("a value never reaches further than its option, and neither does a requirement", "[cli]")
{
    for (const CommandLineOption &option : Compiler::command_line_options()) {
        INFO("--" << option.name);

        REQUIRE((option.required_by & ~option.subcommands) == 0);

        for (const Compiler::OptionValue &value : option.values) {
            INFO(value.name);
            REQUIRE((value.subcommands & ~option.subcommands) == 0);
            REQUIRE(std::string(value.summary).size() > 0);
        }

        REQUIRE(std::string(option.description).size() > 0);
    }
}

// **two texts per row, and the short one has rules.** It is printed into a fixed column beside every
// other option, so a sentence there is a row that wraps and a page that stops being a column
TEST_CASE("every row carries a short summary and a full description", "[cli]")
{
    // the widest a summary may be and still fit the column on an 80-column page
    constexpr size_t LONGEST_SUMMARY = 48;

    const auto check_summary = [](const std::string &what, const char *summary) {
        INFO(what << ": " << summary);

        REQUIRE(summary != nullptr);
        REQUIRE(std::string(summary).find('\n') == std::string::npos);
        REQUIRE(std::string(summary).size() > 0);
        REQUIRE(std::string(summary).size() <= LONGEST_SUMMARY);

        // lowercase and unpunctuated, because it is a label rather than a sentence
        REQUIRE_FALSE(std::isupper(static_cast<unsigned char>(summary[0])));
        REQUIRE(std::string(summary).back() != '.');
    };

    for (const CommandLineOption &option : Compiler::command_line_options()) {
        check_summary(std::string("--") + option.name, option.summary);
        REQUIRE(std::string(option.description).size() > std::string(option.summary).size());

        for (const Compiler::OptionValue &value : option.values) {
            check_summary(value.name, value.summary);
            REQUIRE(std::string(value.description).size() > 0);
        }
    }

    for (const Compiler::SubcommandInfo &info : Compiler::subcommand_table()) {
        check_summary(info.name, info.summary);

        if (info.positional != nullptr) {
            check_summary(info.positional, info.positional_summary);
        }
    }
}

// **the assertion that ties an enumerator to its word.** Without it a reordered table means something
// else in silence: `--print ast` would set PrintKind::t_ir and every reader would be consistent about it
TEST_CASE("a value's code is its enumerator", "[cli]")
{
    const std::vector<Compiler::OptionValue> &prints = Compiler::option_for(Opt::t_print).values;
    REQUIRE(prints.size() == 7);

    REQUIRE(prints[0].code == static_cast<unsigned int>(PrintKind::t_ast));
    REQUIRE(prints[1].code == static_cast<unsigned int>(PrintKind::t_ast_resolved));
    REQUIRE(prints[2].code == static_cast<unsigned int>(PrintKind::t_ir));
    REQUIRE(prints[3].code == static_cast<unsigned int>(PrintKind::t_ir_units));
    REQUIRE(prints[4].code == static_cast<unsigned int>(PrintKind::t_symbols));
    REQUIRE(prints[5].code == static_cast<unsigned int>(PrintKind::t_instances));
    REQUIRE(prints[6].code == static_cast<unsigned int>(PrintKind::t_manifest));

    const std::vector<Compiler::OptionValue> &explains = Compiler::option_for(Opt::t_explain).values;
    REQUIRE(explains.size() == 4);

    REQUIRE(explains[0].code == static_cast<unsigned int>(ExplainKind::t_cache));
    REQUIRE(explains[1].code == static_cast<unsigned int>(ExplainKind::t_prune));
    REQUIRE(explains[2].code == static_cast<unsigned int>(ExplainKind::t_memory));
    REQUIRE(explains[3].code == static_cast<unsigned int>(ExplainKind::t_time));

    const std::vector<Compiler::OptionValue> &modes = Compiler::option_for(Opt::t_optimize).values;
    REQUIRE(modes.size() == 3);

    REQUIRE(modes[0].code == static_cast<unsigned int>(OptimizeMode::t_none));
    REQUIRE(modes[1].code == static_cast<unsigned int>(OptimizeMode::t_module));
    REQUIRE(modes[2].code == static_cast<unsigned int>(OptimizeMode::t_whole));
}

// a retirement sentence that shadowed a live flag would refuse the very spelling it recommends
TEST_CASE("no retired spelling shadows a live one", "[cli]")
{
    for (const CommandLineOption &option : Compiler::command_line_options()) {
        INFO("--" << option.name);

        REQUIRE(Compiler::retired_spelling(std::string("--") + option.name) == nullptr);

        if (option.alias != nullptr) {
            REQUIRE(Compiler::retired_spelling(std::string("--") + option.alias) == nullptr);
        }

        if (option.shorthand != '\0') {
            REQUIRE(Compiler::retired_spelling(std::string("-") + option.shorthand) == nullptr);
        }
    }
}

TEST_CASE("every retired spelling names what to write instead", "[cli]")
{
    const std::vector<const char *> retired = {
        "-O", "--no-optimize", "-a", "--print-ast", "-ar", "--print-resolved-ast", "--print-ir",
        "-pu", "--print-unit-ir", "-syt", "--print-symbol-table", "-pi", "--print-instances",
        "-ec", "--explain-cache", "-ep", "--explain-prune", "-em", "--explain-memory",
        "-t", "--timings", "-ta", "--debug-info", "--stdlib"
    };

    for (const char *word : retired) {
        INFO(word);
        REQUIRE(Compiler::retired_spelling(word) != nullptr);
        REQUIRE(contains(refusal({ "build", word }), "retired"));
    }
}

TEST_CASE("the four value forms parse", "[cli]")
{
    CommandLine cli;
    std::string error;

    // `--long=value` is not a convenience: tests/module_cache.cpp writes `--target-cpu=generic`
    REQUIRE(parse({ "build", "-o", "out", "--target-cpu=generic", "a.eco" }, cli, error));
    REQUIRE(cli.value(Opt::t_target_cpu) == "generic");

    REQUIRE(parse({ "build", "-o", "out", "--target-cpu", "generic", "a.eco" }, cli, error));
    REQUIRE(cli.value(Opt::t_target_cpu) == "generic");

    // an empty value is a real answer - `--target-features=` means "none"
    REQUIRE(parse({ "build", "-o", "out", "--target-features=", "a.eco" }, cli, error));
    REQUIRE(cli.value(Opt::t_target_features).empty());

    // a single dash is a value, so a feature list is writable
    REQUIRE(parse({ "build", "-o", "out", "--target-features", "-crc", "a.eco" }, cli, error));
    REQUIRE(cli.value(Opt::t_target_features) == "-crc");
}

TEST_CASE("a repeated option keeps every word and a plain one keeps the last", "[cli]")
{
    CommandLine cli;
    std::string error;

    REQUIRE(parse({ "clean", "-m", "a", "-m", "b" }, cli, error));
    REQUIRE(cli.list(Opt::t_module) == std::vector<std::string>{ "a", "b" });

    REQUIRE(parse({ "clean", "--build-dir", "x", "--build-dir", "y" }, cli, error));
    REQUIRE(cli.value(Opt::t_build_dir) == "y");
}

TEST_CASE("an unwritten option answers its row's fallback and is not stated", "[cli]")
{
    CommandLine cli;
    std::string error;

    REQUIRE(parse({ "run", "a.eco" }, cli, error));

    REQUIRE_FALSE(cli.stated(Opt::t_optimize));
    REQUIRE(cli.value(Opt::t_optimize) == "module");
    REQUIRE(cli.value(Opt::t_color) == "auto");
    REQUIRE(cli.value(Opt::t_diagnostics) == "auto");
    REQUIRE(cli.value(Opt::t_build_dir).empty());
}

// **`--` means "program arguments", not POSIX's "end of options".** That is this CLI's existing meaning
// and changing it would silently change what `echoc run p.eco -- a b` does
TEST_CASE("the bare -- splits the program's arguments off", "[cli]")
{
    CommandLine cli;
    std::string error;

    REQUIRE(parse({ "run", "p.eco" }, cli, error));
    REQUIRE(cli.program_arguments.empty());

    REQUIRE(parse({ "run", "p.eco", "--" }, cli, error));
    REQUIRE(cli.program_arguments.empty());

    REQUIRE(parse({ "run", "p.eco", "--", "a", "b" }, cli, error));
    REQUIRE(cli.sources == std::vector<std::string>{ "p.eco" });
    REQUIRE(cli.program_arguments == std::vector<std::string>{ "a", "b" });

    // a later `--` is a word of the tail, not a second split
    REQUIRE(parse({ "run", "p.eco", "--", "a", "--", "b" }, cli, error));
    REQUIRE(cli.program_arguments == std::vector<std::string>{ "a", "--", "b" });

    // a flag after the split belongs to the program
    REQUIRE(parse({ "run", "p.eco", "--", "--silent" }, cli, error));
    REQUIRE_FALSE(cli.flag(Opt::t_silent));
    REQUIRE(cli.program_arguments == std::vector<std::string>{ "--silent" });
}

// the parser this replaced declared sources "remaining", so a flag after a filename was read as another
// filename - and the mistake surfaced as a missing source file
TEST_CASE("a positional is recognised at any position", "[cli]")
{
    CommandLine cli;
    std::string error;

    REQUIRE(parse({ "build", "app.eco", "-o", "out" }, cli, error));
    REQUIRE(cli.sources == std::vector<std::string>{ "app.eco" });
    REQUIRE(cli.value(Opt::t_output) == "out");

    REQUIRE(parse({ "build", "-o", "out", "a.eco", "--silent", "b.eco" }, cli, error));
    REQUIRE(cli.sources == std::vector<std::string>{ "a.eco", "b.eco" });
    REQUIRE(cli.flag(Opt::t_silent));
}

// **the page must be reachable from exactly the state that needs it.** A missing -o that complained
// before printing the page would leave `echoc build --help` unable to say what -o is
TEST_CASE("help and version short-circuit every remaining rule", "[cli]")
{
    CommandLine cli;
    std::string error;

    REQUIRE(parse({ "build", "--help" }, cli, error));
    REQUIRE(cli.wants_help);
    REQUIRE(cli.subcommand == Subcommand::t_build);

    REQUIRE(parse({ "--help", "build" }, cli, error));
    REQUIRE(cli.wants_help);
    REQUIRE(cli.subcommand == Subcommand::t_build);

    REQUIRE(parse({ "-h" }, cli, error));
    REQUIRE(cli.wants_help);
    REQUIRE(cli.subcommand == Subcommand::t_none);

    REQUIRE(parse({ "--version" }, cli, error));
    REQUIRE(cli.wants_version);
}

TEST_CASE("a refusal says which mistake was made", "[cli]")
{
    REQUIRE(refusal({}) == "No command given.");

    REQUIRE(refusal({ "compile", "a.eco" })
        == "'compile' is not an echoc command. Write 'run', 'build', 'test', 'clean' or 'lsp'.");

    REQUIRE(refusal({ "run", "--nonsense", "a.eco" }) == "Unknown option '--nonsense'.");

    REQUIRE(refusal({ "build", "-o", "out", "a.eco", "-z" }) == "Unknown option '-z'.");

    // a multi-character short is the mistake somebody coming from the old spellings makes
    REQUIRE(contains(refusal({ "build", "-oout" }), "A short option is one character"));

    REQUIRE(refusal({ "run", "--silent=yes", "a.eco" }) == "'--silent' takes no value.");

    REQUIRE(refusal({ "build", "-o" }) == "'-o, --output <file>' needs a value.");

    REQUIRE(refusal({ "build", "-o", "--silent", "a.eco" })
        == "'-o, --output' needs a value, and '--silent' is an option.");

    // **a missing '-o' is not a parse refusal any more, and that is the assertion.** It was, off the
    // row's `required_by`, for as long as argv held the whole answer - but a manifest declaring targets
    // names its own binaries, so whether one was needed is a question about the project. The parser
    // accepts the words and resolve_programs refuses the invocation, where the manifest is known
    REQUIRE(refusal({ "build", "a.eco" }) == "<accepted>");
    REQUIRE(refusal({ "build", "-o", "out", "a.eco" }) == "<accepted>");
    REQUIRE(refusal({ "build", "-o", "out" }) == "<accepted>");
    REQUIRE(refusal({ "build", "--target", "clock" }) == "<accepted>");

    REQUIRE(refusal({ "clean", "app.eco" })
        == "'clean' takes no source files. It parses none and runs no pass.");

    REQUIRE(refusal({ "lsp" }) == "<accepted>");
    REQUIRE(refusal({ "lsp", "app.eco" })
        == "'lsp' takes no source files. It parses none and runs no pass.");
    REQUIRE(refusal({ "lsp", "--no-stdlib" }) == "<accepted>");
    REQUIRE(refusal({ "lsp", "-m", "lib" }) == "<accepted>");
    REQUIRE(refusal({ "lsp", "--stdio" }) == "<accepted>");
    REQUIRE(refusal({ "run", "--stdio", "a.eco" }) == "'run' does not take '--stdio'.");
    REQUIRE(refusal({ "lsp", "--diagnostics", "json" }) == "'lsp' does not take '--diagnostics'.");

    REQUIRE(refusal({ "build", "-o", "out", "a.eco", "--", "x" })
        == "Only 'run' passes arguments to the program, so '--' means nothing to 'build'.");

    REQUIRE(refusal({ "clean", "--explain", "cache" }) == "'clean' does not take '--explain'.");

    REQUIRE(refusal({ "build", "--timeout", "200", "-o", "out", "a.eco" })
        == "'build' does not take '--timeout'.");

    REQUIRE(refusal({ "test", "--timeout", "200", "a.eco" }) == "<accepted>");

    REQUIRE(contains(refusal({ "test", "--timeout", "soon", "a.eco" }),
        "a timeout is a number of milliseconds"));

    REQUIRE(contains(
        refusal({ "run", "--debug", "--release", "a.eco" }), "two answers to one question"));

    REQUIRE(contains(
        refusal({ "run", "--no-stdlib", "--emit-stdlib-header", "a.eco" }),
        "two answers to one question"));

    REQUIRE(refusal({ "run", "--optimize", "hard", "a.eco" })
        == "Unknown '--optimize' value 'hard'. Expected one of: none|module|whole.");

    // the vocabulary another owner holds still reports through that owner's own table
    REQUIRE(contains(refusal({ "run", "--color", "alwyas", "a.eco" }), "Unknown '--color' value"));
}

// **a flag a subcommand accepts and silently ignores is worse than one it rejects, and a *value* is no
// different.** Only the JIT prunes, so `build --explain prune` is a mistake with a different remedy from
// an unknown word - a different subcommand rather than a different spelling
TEST_CASE("a value legal on one subcommand is refused on another by name", "[cli]")
{
    const std::string message = refusal({ "build", "-o", "out", "--explain", "prune", "a.eco" });

    REQUIRE(contains(message, "'build'"));
    REQUIRE(contains(message, "cache|memory|time"));
    REQUIRE_FALSE(contains(message, "prune."));

    REQUIRE(refusal({ "run", "--explain", "prune", "a.eco" }) == "<accepted>");
}

TEST_CASE("the subcommand decides the build mode, and a flag overrides it", "[cli]")
{
    REQUIRE(resolved({ "run", "a.eco" }).options.mode == Compiler::BuildMode::t_debug);
    REQUIRE(resolved({ "build", "-o", "x", "a.eco" }).options.mode == Compiler::BuildMode::t_release);
    REQUIRE(resolved({ "run", "--release", "a.eco" }).options.mode == Compiler::BuildMode::t_release);
    REQUIRE(resolved({ "build", "-o", "x", "--debug", "a.eco" }).options.mode
        == Compiler::BuildMode::t_debug);
}

// **the case only `stated()` can answer.** `-g` means "unoptimized" unless the optimize mode was written,
// so "it defaulted to module" and "the user asked for module" have to be two different answers
TEST_CASE("debug symbols default the optimize mode but never override a written one", "[cli]")
{
    REQUIRE(resolved({ "build", "-o", "x", "a.eco" }).optimize == OptimizeMode::t_module);
    REQUIRE_FALSE(resolved({ "build", "-o", "x", "a.eco" }).options.no_optimize);

    REQUIRE(resolved({ "build", "-o", "x", "-g", "a.eco" }).optimize == OptimizeMode::t_none);
    REQUIRE(resolved({ "build", "-o", "x", "-g", "a.eco" }).options.no_optimize);

    const DriverOptions stated_whole
        = resolved({ "build", "-o", "x", "-g", "--optimize", "whole", "a.eco" });

    REQUIRE(stated_whole.optimize == OptimizeMode::t_whole);
    REQUIRE_FALSE(stated_whole.options.no_optimize);

    const DriverOptions stated_module
        = resolved({ "build", "-o", "x", "-g", "--optimize", "module", "a.eco" });

    REQUIRE(stated_module.optimize == OptimizeMode::t_module);
    REQUIRE_FALSE(stated_module.options.no_optimize);
}

// **the single assertion standing between this and a silent cache-key change.** A dump changes no
// emitted byte, so it must force the merge without ever reaching Compiler::compute_module_keys
TEST_CASE("printing the IR merges the program but never enters the cache key", "[cli]")
{
    const DriverOptions dumped = resolved({ "build", "-o", "x", "--print", "ir", "a.eco" });

    REQUIRE(dumped.whole_program);
    REQUIRE_FALSE(dumped.optimize_is_whole_program());

    const DriverOptions optimized = resolved({ "build", "-o", "x", "--optimize", "whole", "a.eco" });

    REQUIRE(optimized.whole_program);
    REQUIRE(optimized.optimize_is_whole_program());

    // the JIT holds one module and emits no object, so there is never an artifact to store or reuse
    REQUIRE(resolved({ "run", "a.eco" }).whole_program);
    REQUIRE_FALSE(resolved({ "run", "a.eco" }).optimize_is_whole_program());

    const DriverOptions plain = resolved({ "build", "-o", "x", "a.eco" });

    REQUIRE_FALSE(plain.whole_program);
    REQUIRE_FALSE(plain.optimize_is_whole_program());
}

TEST_CASE("explaining memory implies tracking allocations", "[cli]")
{
    const DriverOptions driver = resolved({ "run", "--explain", "memory", "a.eco" });

    REQUIRE(driver.explains(ExplainKind::t_memory));
    REQUIRE(driver.options.report_allocations);
    REQUIRE(driver.options.track_allocations);

    const DriverOptions counted = resolved({ "run", "--track-allocations", "a.eco" });

    REQUIRE(counted.options.track_allocations);
    REQUIRE_FALSE(counted.options.report_allocations);
}

TEST_CASE("a repeated dump request is every dump rather than the last", "[cli]")
{
    const DriverOptions driver
        = resolved({ "run", "-p", "ast", "-p", "ir", "--explain", "time", "a.eco" });

    REQUIRE(driver.prints(PrintKind::t_ast));
    REQUIRE(driver.prints(PrintKind::t_ir));
    REQUIRE_FALSE(driver.prints(PrintKind::t_symbols));
    REQUIRE(driver.explains(ExplainKind::t_time));
    REQUIRE_FALSE(driver.explains(ExplainKind::t_cache));
}

TEST_CASE("manifest is an answer and cannot be combined with another dump", "[cli]")
{
    CommandLine cli;
    std::string error;
    REQUIRE(parse({ "build", "-p", "manifest", "-p", "ast", "a.eco" }, cli, error));

    DriverOptions mixed;
    REQUIRE_FALSE(Compiler::resolve_driver_options(cli, mixed, error));
    REQUIRE(error.find("cannot be combined") != std::string::npos);

    const DriverOptions driver = resolved({ "build", "-p", "manifest", "a.eco" });
    REQUIRE(driver.prints(PrintKind::t_manifest));
}

// **structure rather than a golden, and deliberately.** A two-hundred-line string literal would make
// every wording improvement a test edit, which trains people to regenerate rather than read - and the
// descriptions being prose somebody improves is the whole point of moving them into the table
TEST_CASE("a page shows every option its subcommand accepts and no other", "[cli]")
{
    for (const Compiler::SubcommandInfo &info : Compiler::subcommand_table()) {
        const std::string text = page(info.id, TerminalCapabilities::plain());

        for (const CommandLineOption &option : Compiler::command_line_options()) {
            // **the trailing space is load-bearing**: one option's spelling can be a prefix of
            // another's, and a page carrying `--target-os` would otherwise read as one carrying
            // `--target`. A heading is always followed by one - by its metavar, or by the padding
            // before its summary - so this is the terminator rather than a hopeful guess
            const std::string heading = "  " + Compiler::option_flag_names(option) + " ";
            const bool accepted = (option.subcommands & info.bit) != 0;

            INFO(info.name << " / --" << option.name);

            if (accepted) {
                REQUIRE(contains(text, heading));
            }
            else {
                // the *heading*, not the bare name: a description legitimately mentions another flag,
                // and `--silent` names both --print and --explain in its own paragraph
                REQUIRE_FALSE(contains(text, heading));
            }
        }
    }
}

TEST_CASE("a category with nothing in it is absent from the page", "[cli]")
{
    const std::string build = page(Subcommand::t_build, TerminalCapabilities::plain());
    const std::string clean = page(Subcommand::t_clean, TerminalCapabilities::plain());

    REQUIRE_FALSE(contains(build, Compiler::category_heading(OptionCategory::t_removal)));
    REQUIRE(contains(clean, Compiler::category_heading(OptionCategory::t_removal)));

    // clean compiles nothing, so it answers no question about how
    REQUIRE_FALSE(contains(clean, Compiler::category_heading(OptionCategory::t_build)));
    REQUIRE(contains(build, Compiler::category_heading(OptionCategory::t_build)));
}

// **a value a subcommand cannot write is shown and marked, never hidden.** A reader who cannot see
// `prune` on the build page cannot learn it exists; writing it there is still the refusal naming `run`
TEST_CASE("a page lists every value, marking the ones this subcommand cannot write", "[cli]")
{
    const std::string build = page(Subcommand::t_build, TerminalCapabilities::plain());
    const std::string run = page(Subcommand::t_run, TerminalCapabilities::plain());

    for (const char *value : { "cache", "prune", "memory", "time" }) {
        REQUIRE(contains(build, value));
        REQUIRE(contains(run, value));
    }

    // **every command that can answer it, named** - the marker is the remedy and not only a refusal, so
    // opening `prune` up to `test` changed this word rather than needing a second marker
    REQUIRE(contains(build, "(run/test only)"));
    REQUIRE_FALSE(contains(run, "(run/test only)"));

    // the marker is on the value it is about, not merely somewhere on the page
    const size_t at = build.find("prune");
    REQUIRE(at != std::string::npos);
    REQUIRE(contains(build.substr(at, build.find('\n', at) - at), "(run/test only)"));

    // and the default is named on the value row itself - located by its connector, because "module"
    // also appears in `-m, --module` further up the page
    const size_t row = build.find("- module");
    REQUIRE(row != std::string::npos);
    REQUIRE(contains(build.substr(row, build.find('\n', row) - row), "(default)"));
}

// the page hangs an option's values off it, and the connectors are the one thing on it that a terminal
// may be unable to draw - so they are a theme, the way AST::DiagnosticTheme already is
TEST_CASE("the value tree is drawn with the terminal's own repertoire", "[cli]")
{
    const std::string unicode = page(Subcommand::t_run, a_terminal(true, 80, false));
    const std::string ascii = page(Subcommand::t_run, TerminalCapabilities::plain());

    REQUIRE(contains(unicode, "\u251c\u2500 ast"));
    REQUIRE(contains(unicode, "\u251c\u2500 instances"));
    REQUIRE(contains(unicode, "\u2514\u2500 manifest"));

    REQUIRE(contains(ascii, "|- ast"));
    REQUIRE(contains(ascii, "|- instances"));
    REQUIRE(contains(ascii, "`- manifest"));

    // the plain page is ascii to the byte, so a golden or a CI log never carries a glyph
    for (const unsigned char byte : ascii) {
        REQUIRE(byte < 0x80);
    }
}

TEST_CASE("prepare_terminal is safe to call twice", "[cli]")
{
    Compiler::prepare_terminal();
    Compiler::prepare_terminal();
}

#if defined(_WIN32)
TEST_CASE("prepare_terminal sets an attached console to UTF-8", "[cli]")
{
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (err == INVALID_HANDLE_VALUE || err == nullptr || GetConsoleMode(err, &mode) == 0) {
        SKIP("stderr is not a console");
    }

    Compiler::prepare_terminal();
    REQUIRE(GetConsoleOutputCP() == CP_UTF8);
}
#endif

TEST_CASE("a heading is drawn, not shouted", "[cli]")
{
    const std::string unicode = page(Subcommand::t_none, a_terminal(true, 80, false));
    const std::string ascii = page(Subcommand::t_none, TerminalCapabilities::plain());

    // U+258C LEFT HALF BLOCK, then the word, then the rule of U+2500. The ascii theme draws neither
    // glyph, so the heading is the word alone rather than a row of hyphens pretending to be a rule
    REQUIRE(contains(unicode, "\u258c Usage "));
    REQUIRE(contains(unicode, "\u2500"));

    REQUIRE(contains(ascii, "Usage\n"));
    REQUIRE_FALSE(contains(ascii, "\u258c"));
    REQUIRE_FALSE(contains(ascii, "\u2500"));
}

// **summaries form a column that can be read down.** One width per page, so a row that escaped it would
// be the row that breaks the whole page
TEST_CASE("every summary on a page starts at the same column", "[cli]")
{
    for (const Compiler::SubcommandInfo &info : Compiler::subcommand_table()) {
        size_t column = 0;

        for (const std::string &line : lines_of(page(info.id, TerminalCapabilities::plain()))) {
            // a row is a line that opens with the page indent and carries a summary after a gutter
            if (line.rfind("  ", 0) != 0 || line.rfind("   ", 0) == 0) {
                continue;
            }

            const size_t gutter = line.find("  ", 2);
            if (gutter == std::string::npos) {
                continue;
            }

            const size_t at = line.find_first_not_of(' ', gutter);
            if (at == std::string::npos) {
                continue;
            }

            INFO(info.name << ": " << line);

            if (column == 0) {
                column = at;
            }

            REQUIRE(at == column);
        }

        REQUIRE(column > 0);
    }
}

TEST_CASE("a page wraps to its width and never trails whitespace", "[cli]")
{
    for (const unsigned int width : { 0u, 40u, 80u, 200u }) {
        // width 0 is a stream with no window, which gets 80; 200 is capped at 100
        const unsigned int limit = width == 0 ? 80 : (width > 100 ? 100 : width);

        for (const Compiler::SubcommandInfo &info : Compiler::subcommand_table()) {
            for (const std::string &line : lines_of(page(info.id, a_terminal(true, width, false)))) {
                INFO(info.name << " @" << width << ": " << line);

                REQUIRE((line.empty() || line.back() != ' '));

                // **a usage line and an option heading are exempt, and they are the only things that
                // are.** Each is one unbreakable construct: `echoc build [options] -o <file>` and
                // `-p, --print <ast|ast-resolved|...>` read worse across two lines than off the edge of a
                // narrow one, and a flag name cannot be hyphenated. Every paragraph on the page - which
                // is the bulk of it - still has to fit
                if (line.rfind("  echoc ", 0) == 0 || line.rfind("  -", 0) == 0) {
                    continue;
                }

                REQUIRE(display_width(line) <= limit);
            }
        }
    }
}

// **strictly stronger than writing the golden twice.** The coloured page and the plain one are the same
// bytes with escape sequences interleaved, which is what lets this renderer take no theme
TEST_CASE("colour adds escapes and changes nothing else", "[cli]")
{
    for (const Compiler::SubcommandInfo &info : Compiler::subcommand_table()) {
        const std::string plain = page(info.id, a_terminal(true, 80, false));
        const std::string colored = page(info.id, a_terminal(true, 80, true));

        INFO(info.name);

        REQUIRE(plain.find('\x1b') == std::string::npos);
        REQUIRE(colored.find('\x1b') != std::string::npos);
        REQUIRE(without_escapes(colored) == plain);
    }
}

TEST_CASE("the usage line is built from the table", "[cli]")
{
    std::ostringstream out;
    CommandLineHelp(out, TerminalCapabilities::plain()).render_usage(Subcommand::t_none);

    // **`build` carries no required option any more**, and this line reading the table is how that shows:
    // `-o` stopped being answerable from argv alone the moment a manifest could declare its own targets
    // and name its own binaries, so the row's `required_by` went to zero and the usage line followed
    REQUIRE(out.str() ==
        "Usage\n"
        "  echoc <command> [options] [sources...]\n"
        "\n"
        "  echoc run [options] <sources...> [-- <program arguments>]\n"
        "  echoc build [options] <sources...>\n"
        "  echoc test [options] <sources...>\n"
        "  echoc clean [options]\n"
        "  echoc lsp [options]\n");
}

TEST_CASE("a refusal is the sentence, the usage and where to read more", "[cli]")
{
    std::ostringstream out;
    CommandLineHelp(out, TerminalCapabilities::plain())
        .render_error("'build' needs '-o, --output <file>'.", Subcommand::t_build);

    REQUIRE(out.str() ==
        "error: 'build' needs '-o, --output <file>'.\n"
        "\n"
        "Usage\n"
        "  echoc build [options] <sources...>\n"
        "\n"
        "Run 'echoc build --help' for what this command accepts.\n");
}

// **the prose is one command away, and the page says so.** The full description was written once, into
// the table, and a description nobody can reach is a description nobody wrote
TEST_CASE("`--help` names one option and prints it in full", "[cli]")
{
    CommandLine cli;
    std::string error;

    REQUIRE(parse({ "build", "--help", "optimize" }, cli, error));
    REQUIRE(cli.wants_help);
    REQUIRE(cli.help_topic == &Compiler::option_for(Opt::t_optimize));

    // a lone character answers the short spelling. **written with its dashes it cannot**, and that is
    // not a gap: `--help --optimize` reads `--optimize` as the option itself, which is why the topic is
    // the bare name
    REQUIRE(parse({ "build", "--help", "g" }, cli, error));
    REQUIRE(cli.help_topic == &Compiler::option_for(Opt::t_debug_symbols));

    // **a word that names no option is not a mistake.** Somebody who typed `--help main.eco` asked for
    // help, and refusing them is the second-worst thing this command line could do with that flag
    REQUIRE(parse({ "run", "--help", "main.eco" }, cli, error));
    REQUIRE(cli.wants_help);
    REQUIRE(cli.help_topic == nullptr);

    REQUIRE(parse({ "run", "--help", "a.eco", "b.eco" }, cli, error));
    REQUIRE(cli.help_topic == nullptr);

    // naming an option this command does not take is, though - and the sentence says which does
    REQUIRE(refusal({ "build", "--help", "with-stdlib" }) == "'build' does not take '--with-stdlib'.");
}

TEST_CASE("one option's page is that option and nothing else", "[cli]")
{
    std::ostringstream out;
    CommandLineHelp(out, TerminalCapabilities::plain())
        .render_option_help(Subcommand::t_build, Compiler::option_for(Opt::t_optimize));

    const std::string text = out.str();

    // the full prose, which the compact page deliberately does not carry. Asserted on a phrase rather
    // than the whole string, because the renderer wraps it - a byte comparison would be a golden of the
    // wording, which is the thing this suite deliberately does not pin
    REQUIRE(contains(text, "one option with three values now"));
    REQUIRE_FALSE(contains(text, Compiler::option_for(Opt::t_optimize).summary));

    // every value, with its own description rather than the page's one-liner - the first words of each,
    // for the wrapping reason above
    for (const Compiler::OptionValue &value : Compiler::option_for(Opt::t_optimize).values) {
        INFO(value.name);
        REQUIRE(contains(text, value.name));
        REQUIRE(contains(text, std::string(value.description).substr(0, 24)));
    }

    // the facts the prose must not have to repeat, because they are on the row and would go stale
    REQUIRE(contains(text, "Accepted by: run, build, test."));
    REQUIRE(contains(text, "Defaults to 'module'."));

    // and no other option leaks onto it
    REQUIRE_FALSE(contains(text, "--track-allocations"));
}

// **which stream a thing is on is what the e2e corpus rests on**, and only a real process can pin it.
// `echoc --version` is also a release-workflow contract: it is string-compared against the resolved tag
TEST_CASE("help answers on stdout and a refusal on stderr", "[cli]")
{
    const std::string echoc = EchoTests::quoted(ECHOC_BINARY);

#if defined(_WIN32)
    const char *quiet_stdout = " 2>nul";
    const char *stderr_only = " 2>&1 1>nul";
#else
    const char *quiet_stdout = " 2>/dev/null";
    const char *stderr_only = " 2>&1 1>/dev/null";
#endif

    const EchoTests::ProcessResult version = EchoTests::run_capturing(echoc + " --version" + quiet_stdout);
    REQUIRE(version.exit_code == 0);
    REQUIRE(version.output == std::string(ECO_VERSION_STRING) + "\n");

    // the page goes to stdout and writes nothing to the diagnostic stream
    const EchoTests::ProcessResult help = EchoTests::run_capturing(echoc + " --help" + quiet_stdout);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.output.find("Usage") != std::string::npos);

    const EchoTests::ProcessResult help_stderr
        = EchoTests::run_capturing(echoc + " --help" + stderr_only);
    REQUIRE(help_stderr.output.empty());

    // and a refusal is the mirror of it: stderr, exit 1, nothing on stdout
    const EchoTests::ProcessResult bare = EchoTests::run_capturing(echoc + quiet_stdout);
    REQUIRE(bare.exit_code == 1);
    REQUIRE(bare.output.empty());

    const EchoTests::ProcessResult bare_stderr
        = EchoTests::run_capturing(echoc + stderr_only);
    REQUIRE(bare_stderr.output.find("No command given.") != std::string::npos);
}
