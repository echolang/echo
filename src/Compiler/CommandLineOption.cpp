#include "Compiler/CommandLineOption.h"

#include "Compiler/TerminalCapabilities.h"

#include <cassert>
#include <cstddef>
#include <utility>

namespace
{
    // the two flag parsers that already own their own vocabularies, adapted to OptionCheck. They report
    // their own "Expected one of: ..." out of their own tables, which is why those values are not listed
    // a second time in the rows below
    bool check_color(const std::string &value, std::string &out_error)
    {
        Compiler::ColorChoice choice = Compiler::ColorChoice::t_auto;
        return Compiler::parse_color_choice(value, choice, out_error);
    }

    bool check_diagnostics(const std::string &value, std::string &out_error)
    {
        Compiler::DiagnosticFormat format = Compiler::DiagnosticFormat::t_auto;
        return Compiler::parse_diagnostic_format(value, format, out_error);
    }

    unsigned int code_of(Compiler::PrintKind kind)
    {
        return static_cast<unsigned int>(kind);
    }

    unsigned int code_of(Compiler::ExplainKind kind)
    {
        return static_cast<unsigned int>(kind);
    }

    unsigned int code_of(Compiler::OptimizeMode mode)
    {
        return static_cast<unsigned int>(mode);
    }

    // `<none|module|whole>` for an option that owns its vocabulary, the written metavar otherwise. one
    // place, so a value added to a row shows up in every usage line without a second string being edited
    std::string metavar_of(const Compiler::CommandLineOption &option)
    {
        if (!Compiler::option_takes_a_value(option)) {
            return "";
        }

        if (option.metavar != nullptr) {
            return option.metavar;
        }

        std::string joined = "<";

        for (const Compiler::OptionValue &value : option.values) {
            if (joined.size() > 1) {
                joined += "|";
            }

            joined += value.name;
        }

        return joined + ">";
    }
};

unsigned int Compiler::bit_of(Subcommand id)
{
    switch (id) {
    case Subcommand::t_run:
        return accepts::run;
    case Subcommand::t_build:
        return accepts::build;
    case Subcommand::t_clean:
        return accepts::clean;
    case Subcommand::t_none:
        return 0;
    }

    return 0;
}

const char *Compiler::category_heading(OptionCategory category)
{
    switch (category) {
    case OptionCategory::t_inputs:
        return "What is built";
    case OptionCategory::t_build:
        return "How it is built";
    case OptionCategory::t_target:
        return "What it is built for";
    case OptionCategory::t_report:
        return "What echoc tells you";
    case OptionCategory::t_removal:
        return "What clean removes";
    case OptionCategory::t_general:
        return "General";
    }

    return "";
}

const char *Compiler::exclusion_group_name(ExclusionGroup group)
{
    switch (group) {
    case ExclusionGroup::t_build_mode:
        return "build mode";
    case ExclusionGroup::t_stdlib_use:
        return "standard library";
    case ExclusionGroup::t_none:
        return "";
    }

    return "";
}

// **the table.** one row per option, in the order a help page lists them within their category.
//
// built once on first call rather than as a namespace-scope object, because the rows hold a
// std::vector<OptionValue> and static initialisation order across translation units is not a thing to
// bet a command line on
const std::vector<Compiler::CommandLineOption> &Compiler::command_line_options()
{
    static const std::vector<CommandLineOption> table = {
        {
            Opt::t_output, "output", nullptr, 'o',
            OptionArity::t_value, OptionCategory::t_inputs,
            accepts::build, accepts::build, ExclusionGroup::t_none,
            "<file>", "",
            "where the executable is written",
            "Where the executable is written. Required, and refused up front rather than after a "
            "compile: the build has nowhere to put what it made, and finding that out last wastes "
            "every second of it.",
            {}, nullptr
        },
        {
            Opt::t_module, "module", nullptr, 'm',
            OptionArity::t_repeated_value, OptionCategory::t_inputs,
            accepts::all, 0, ExclusionGroup::t_none,
            "<manifest>", "",
            "build a module from its manifest",
            "Build a module from its manifest. May be given more than once; a manifest may be a "
            "'module.eco' file or the directory holding one.\n"
            "The graph is resolved as a whole, so naming a library that depends on another pulls the "
            "other in too, once, ahead of it - a dependency does not have to be spelled here. Root "
            "order is authoritative, never the order they sit in on disk.",
            {}, nullptr
        },
        {
            Opt::t_build_dir, "build-dir", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_inputs,
            accepts::all, 0, ExclusionGroup::t_none,
            "<dir>", "",
            "where build artifacts are written",
            "Where build artifacts are written. Defaults to 'ecobuild' beside each manifest, or to "
            "whatever that manifest's '#[build_dir: ...]' names. One directory for the whole build, "
            "with a subdirectory per module.\n"
            "A directory named here is one a person chose, so it has to carry echoc's CACHEDIR.TAG "
            "before anything is written into it or removed from it. That is what keeps 'clean' from "
            "being a command that can empty somebody's source tree.",
            {}, nullptr
        },
        {
            Opt::t_link, "link", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_inputs,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<requirement>", "",
            "add a link requirement",
            "Add a link requirement: 'lib:<name>', 'framework:<name>', 'search:<dir>' or "
            "'object:<file>'. May be given more than once.\n"
            "The pressure valve, and deliberately the same closed vocabulary a manifest writes. Where "
            "a library is installed is a property of the machine rather than of the module, so there "
            "has to be a way to reach one nothing declared - but a second spelling would be two things "
            "to keep correct, and the one used least is the one that drifts. Merged after every "
            "manifest's, so a declaration wins and a search path given here is consulted last.",
            {}, nullptr
        },
        {
            Opt::t_debug, "debug", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_build_mode,
            nullptr, "",
            "keep 'assert' and the runtime checks",
            "Keep 'assert' and the compiler's own runtime checks - the null check a 'ptr<T>' to 'T&' "
            "narrowing emits, and whatever check comes next. The default for 'run'.\n"
            "This is about the checks the *program* carries and says nothing about optimization or "
            "about what a debugger can see; --optimize and --debug-symbols are the other two axes.",
            {}, nullptr
        },
        {
            Opt::t_release, "release", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_build_mode,
            nullptr, "",
            "drop 'assert' and the runtime checks",
            "Drop 'assert' and the compiler's own runtime checks. The default for 'build'.",
            {}, nullptr
        },
        {
            Opt::t_optimize, "optimize", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<mode>", "module",
            "how hard, and over how much at once",
            "How hard the optimizer works, and over how much of the program at once.\n"
            "One option and not two: the pair this replaced was '-O' and '--no-optimize', which sound "
            "like each other's inverse and were not - one said what is compiled together and the other "
            "whether the per-unit pipeline ran at all, so writing both was expressible and meant almost "
            "nothing.",
            {
                {
                    "none", accepts::compiling, code_of(OptimizeMode::t_none),
                    "skip the per-unit pipeline",
                    "Skip the per-unit pipeline an ordinary build runs, and emit unoptimized IR. For reading raw "
                    "IR, and for bisecting a miscompile against the optimizer."
                },
                {
                    "module", accepts::compiling, code_of(OptimizeMode::t_module),
                    "optimize each unit on its own",
                    "Run the baseline pipeline over each unit on its own. The only mode that leaves a per-module "
                    "object there is any point storing, which is why it is the default."
                },
                {
                    "whole", accepts::compiling, code_of(OptimizeMode::t_whole),
                    "merge every unit, then optimize",
                    "Fold every unit into one module and optimize the whole program. The optimizer can only inline "
                    "a body it can see - and after the merge there are no per-module objects left, so this "
                    "bypasses the object cache. '#[inline]' is how a function stays inlinable without it."
                }
            },
            nullptr
        },
        {
            Opt::t_debug_symbols, "debug-symbols", nullptr, 'g',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            nullptr, "",
            "emit DWARF, so a debugger can step it",
            "Emit DWARF, so the program can be stepped through in a debugger.\n"
            "A third axis, orthogonal to --debug/--release: that pair decides which checks the program "
            "carries, this decides what the object tells a debugger. Folding them would make an "
            "assertion-free build undebuggable by construction, which is exactly the build somebody "
            "reaches for a debugger over.\n"
            "It sets --optimize to none unless --optimize was written, because a line table over "
            "optimized output describes a program nobody wrote. 'run' accepts it and says it cannot "
            "honour it - the JIT emits no object for a debugger to open.",
            {}, nullptr
        },
        {
            Opt::t_no_tbaa, "no-tbaa", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            nullptr, "",
            "emit no type-based alias metadata",
            "Emit no type-based alias metadata.\n"
            "Not a tuning knob: a defined program must produce the same result with it and without it, "
            "and a program where the two disagree has either found a bug in the access-family table or "
            "made a false 'unsafe' promotion. That equivalence is the whole reason the flag exists.",
            {}, nullptr
        },
        {
            Opt::t_track_allocations, "track-allocations", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            nullptr, "",
            "count outstanding allocations",
            "Count how many allocations are outstanding, so 'mem::live_allocations()' can be read from "
            "Echo. Without it that builtin is refused rather than answered 0.\n"
            "Its own option rather than a consequence of --debug, and that is the point: a debug build "
            "is about the checks the program owes itself, and this is bookkeeping somebody asked for. "
            "Tying it to the build mode would make the emitted allocation call spell itself "
            "differently in the two modes, which quietly weakens every IR golden written against the "
            "wrong one.",
            {}, nullptr
        },
        {
            Opt::t_no_stdlib, "no-stdlib", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_stdlib_use,
            nullptr, "",
            "compile without the standard library",
            "Compile without the standard library. 'array', 'string', 'die', 'assert' and the "
            "'contract::', 'mem::', 'str::', 'arr::', 'hash::' and 'std::' namespaces are then simply "
            "undeclared names.\n"
            "The standard library is a module like any other, so it can be left out.",
            {}, nullptr
        },
        {
            Opt::t_emit_stdlib_header, "emit-stdlib-header", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_stdlib_use,
            nullptr, "",
            "regenerate the embedded stdlib header",
            "Regenerate 'stdlib/build/stdlib_embedded.h' from the standard library sources. It embeds "
            "the bytes rather than a file list, so it has to be regenerated when a stdlib file is "
            "edited and not only when one is added.\n"
            "Rewriting a tracked file is opt-in rather than a side effect of every compile. Refused "
            "beside --no-stdlib, because there is then no standard library to write out.",
            {}, nullptr
        },
        {
            Opt::t_target_os, "target-os", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::all, 0, ExclusionGroup::t_none,
            "<name>", "",
            "evaluate '#[if:]' as if targeting this OS",
            "Evaluate '#[if: os == ...]' conditions as if targeting this OS. Does not "
            "cross-compile - the code is still compiled for this machine, so asking for a foreign OS "
            "will usually fail at link.\n"
            "It exists so a test can assert what another platform's branch does without owning that "
            "platform, which is the only way an '#[if: os == linux]' region is ever checked on a Mac. "
            "'clean' reads it too, because a manifest may gate its '#[depends:]' behind a condition - "
            "so the graph reached without it is not the graph the build produced.",
            {}, nullptr
        },
        {
            Opt::t_target_arch, "target-arch", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::all, 0, ExclusionGroup::t_none,
            "<name>", "",
            "the same for the architecture",
            "Evaluate conditions as if targeting this architecture. Does not cross-compile.",
            {}, nullptr
        },
        {
            Opt::t_define, "define", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_target,
            accepts::all, 0, ExclusionGroup::t_none,
            "<name>", "",
            "declare a flag '#[if: NAME]' can test",
            "Declare a flag that '#[if: NAME]' can test. May be given more than once.\n"
            "Bare names only: naming a *value* is what 'const' declarations are for. An undefined flag "
            "is false rather than an error - the opposite of the os and arch vocabularies, which are "
            "closed, because '#[if: TRACE]' could otherwise never take its else arm.",
            {}, nullptr
        },
        {
            Opt::t_target_cpu, "target-cpu", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<name>", "",
            "which CPU to select instructions for",
            "Which CPU to optimize and select instructions for. Defaults to the platform baseline for "
            "this target.\n"
            "The other kind of target flag entirely, and a neighbour of --target-os only because they "
            "start with the same word: that one changes what a condition sees, this changes what is "
            "emitted - every cost model in the optimizer and instruction selection itself are "
            "downstream of it.\n"
            "Never the host by default. 'native' means this machine, and its output may not run on the "
            "machine next to it - with an illegal instruction rather than a diagnostic - so it has to "
            "be asked for by name.",
            {}, nullptr
        },
        {
            Opt::t_target_features, "target-features", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<list>", "",
            "features to enable or disable",
            "Target features to enable or disable, as '+sve,-crc'. Empty unless asked for.",
            {}, nullptr
        },
        {
            Opt::t_print, "print", nullptr, 'p',
            OptionArity::t_repeated_value, OptionCategory::t_report,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<what>", "",
            "dump what the compiler built, by layer",
            "Dump what the compiler built, by layer. May be given more than once, so "
            "'-p ast -p ir' is two dumps rather than the last one.\n"
            "Each writes a '[section]' header, so the output is greppable and stable enough to assert "
            "on.",
            {
                {
                    "ast", accepts::compiling, code_of(PrintKind::t_ast),
                    "the AST as parsed",
                    "The AST as parsed, before the semantic passes."
                },
                {
                    "ast-resolved", accepts::compiling, code_of(PrintKind::t_ast_resolved),
                    "the AST after the semantic passes",
                    "The AST after the semantic passes. Every deref, drop, retain and release is a node here that "
                    "does not exist in 'ast', so a reference count is checked by reading this one."
                },
                {
                    "ir", accepts::compiling, code_of(PrintKind::t_ir),
                    "the merged whole-program LLVM IR",
                    "The LLVM IR, as one whole-program module. Printing it implies the merge, because a single dump "
                    "can only look at one module - so this always describes the whole-program build even with no "
                    "other flag set."
                },
                {
                    "ir-units", accepts::compiling, code_of(PrintKind::t_ir_units),
                    "each unit, as the object writer gets it",
                    "Each compilation unit's IR separately, as the object writer receives it. The per-module path, "
                    "and the only way to assert on what an ordinary build actually emits."
                },
                {
                    "symbols", accepts::compiling, code_of(PrintKind::t_symbols),
                    "the registered symbol table",
                    "The registered symbol table: the namespace tree that holds the types, and the function "
                    "registry that holds the overload sets."
                },
                {
                    "instances", accepts::compiling, code_of(PrintKind::t_instances),
                    "generic instances and rewired calls",
                    "The monomorphizer's instances, rewired call sites and struct instantiations. Reach for this "
                    "one first when a generic goes wrong."
                }
            },
            nullptr
        },
        {
            Opt::t_explain, "explain", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_report,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<what>", "",
            "explain a decision the compiler made",
            "Explain a decision the compiler made. May be given more than once.\n"
            "Measure, don't guess. These are ordinary diagnostics rather than debugging leftovers: the "
            "first run of 'time' found that expanding the standard library's source globs cost more "
            "than lexing all of it. Each writes a '[section]' header.",
            {
                {
                    "cache", accepts::compiling, code_of(ExplainKind::t_cache),
                    "each module's key, and what changed",
                    "Each module's cache key, whether its artifact is present, and on a miss which input changed."
                },
                {
                    "prune", accepts::run, code_of(ExplainKind::t_prune),
                    "what the JIT prune dropped",
                    "How many function definitions the JIT prune dropped, and which ones the entry point still "
                    "reaches. 'run' only, because only the JIT prunes - 'build' refuses it rather than accepting "
                    "a flag it would silently ignore."
                },
                {
                    "memory", accepts::compiling, code_of(ExplainKind::t_memory),
                    "live allocations when the program ended",
                    "Make the compiled program print how many allocations were still outstanding when it ended. "
                    "Unlike the other three this changes the emitted program rather than reporting what echoc "
                    "did, and it implies --track-allocations."
                },
                {
                    "time", accepts::compiling, code_of(ExplainKind::t_time),
                    "where the compile spent its time",
                    "Where the compile spent its time, by phase, as a tree."
                }
            },
            nullptr
        },
        {
            Opt::t_diagnostics, "diagnostics", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_report,
            accepts::all, 0, ExclusionGroup::t_none,
            "<auto|pretty|ascii|json>", "auto",
            "how a diagnostic is drawn",
            "How a diagnostic is drawn: auto, pretty, ascii, or json - one object per line, which is "
            "what editor tooling reads.\n"
            "'auto' picks the drawn form when stderr can render it and the plain one otherwise, which "
            "is what makes a CI log, a Windows console and a golden test all get the same ASCII with "
            "nobody configuring anything. Diagnostics go to stderr whichever form is chosen.",
            {}, check_diagnostics
        },
        {
            Opt::t_color, "color", "colour", '\0',
            OptionArity::t_value, OptionCategory::t_report,
            accepts::all, 0, ExclusionGroup::t_none,
            "<auto|always|never>", "auto",
            "colourise diagnostics",
            "Colourise diagnostics. 'always' into a pipe is a request for escape sequences on purpose, "
            "which is what a job that renders them out of a stored log wants.\n"
            "NO_COLOR, CLICOLOR_FORCE and TERM=dumb are honoured.",
            {}, check_color
        },
        {
            Opt::t_silent, "silent", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_report,
            accepts::all, 0, ExclusionGroup::t_none,
            nullptr, "",
            "do not draw the progress checklist",
            "Do not draw the progress checklist.\n"
            "It silences that and nothing else: diagnostics, warnings and every --print and --explain "
            "dump were asked for by name, and a flag that could be ignored because another flag was "
            "set is a flag nobody can rely on.\n"
            "There is deliberately no --progress beside it. The checklist is drawn only when stderr is "
            "a terminal that can be redrawn, which is the same rule '--diagnostics=auto' follows and "
            "for the same reason; --color=always exists because a job legitimately renders escape "
            "sequences out of a stored log, and nobody wants a carriage return in one.",
            {}, nullptr
        },
        {
            Opt::t_with_stdlib, "with-stdlib", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_removal,
            accepts::clean, 0, ExclusionGroup::t_none,
            nullptr, "",
            "also remove the standard library's store",
            "Also remove the standard library's store. It is shared by every project on this machine "
            "and is the slowest thing in any build to produce again, so a project's clean leaves it "
            "alone.",
            {}, nullptr
        },
        {
            Opt::t_dry_run, "dry-run", nullptr, 'n',
            OptionArity::t_flag, OptionCategory::t_removal,
            accepts::clean, 0, ExclusionGroup::t_none,
            nullptr, "",
            "print what would be removed, remove nothing",
            "Print what would be removed, and remove nothing.",
            {}, nullptr
        },
        {
            Opt::t_help, "help", nullptr, 'h',
            OptionArity::t_flag, OptionCategory::t_general,
            accepts::all, 0, ExclusionGroup::t_none,
            nullptr, "",
            "print this page, or one option in full",
            "Print this page. Given with a command - 'echoc build --help' - print that command's "
            "page instead.",
            {}, nullptr
        },
        {
            Opt::t_version, "version", nullptr, 'v',
            OptionArity::t_flag, OptionCategory::t_general,
            accepts::all, 0, ExclusionGroup::t_none,
            nullptr, "",
            "print the version",
            "Print the version and exit.",
            {}, nullptr
        }
    };

    // the identity option_for relies on, asserted where the table is built rather than trusted at every
    // lookup. tests/command_line.cpp pins it too, for a release build where this does not run
    assert(table.size() == static_cast<size_t>(Opt::t_version) + 1 && "an Opt has no row in the table");

    return table;
}

const Compiler::CommandLineOption &Compiler::option_for(Opt id)
{
    const std::vector<CommandLineOption> &table = command_line_options();
    const size_t index = static_cast<size_t>(id);

    assert(index < table.size() && table[index].id == id && "the option table is out of order");

    return table[index];
}

const Compiler::CommandLineOption *Compiler::option_for_long(const std::string &name)
{
    for (const CommandLineOption &option : command_line_options()) {
        if (name == option.name) {
            return &option;
        }

        if (option.alias != nullptr && name == option.alias) {
            return &option;
        }
    }

    return nullptr;
}

const Compiler::CommandLineOption *Compiler::option_for_short(char shorthand)
{
    if (shorthand == '\0') {
        return nullptr;
    }

    for (const CommandLineOption &option : command_line_options()) {
        if (option.shorthand == shorthand) {
            return &option;
        }
    }

    return nullptr;
}

bool Compiler::option_takes_a_value(const CommandLineOption &option)
{
    return option.arity != OptionArity::t_flag;
}

std::string Compiler::option_flag_names(const CommandLineOption &option)
{
    std::string names;

    if (option.shorthand != '\0') {
        names += "-";
        names += option.shorthand;
        names += ", ";
    }

    names += "--";
    names += option.name;

    if (option.alias != nullptr) {
        names += ", --";
        names += option.alias;
    }

    return names;
}

std::string Compiler::option_spelling(const CommandLineOption &option)
{
    const std::string metavar = metavar_of(option);

    if (metavar.empty()) {
        return option_flag_names(option);
    }

    return option_flag_names(option) + " " + metavar;
}

std::string Compiler::option_value_list(const CommandLineOption &option, Subcommand subject)
{
    const unsigned int wanted = subject == Subcommand::t_none ? ~0u : bit_of(subject);
    std::string joined;

    for (const OptionValue &value : option.values) {
        if ((value.subcommands & wanted) == 0) {
            continue;
        }

        if (!joined.empty()) {
            joined += "|";
        }

        joined += value.name;
    }

    return joined;
}

const Compiler::OptionValue *Compiler::option_value_for_code(
    const CommandLineOption &option,
    unsigned int code
)
{
    for (const OptionValue &value : option.values) {
        if (value.code == code) {
            return &value;
        }
    }

    return nullptr;
}

const std::vector<Compiler::SubcommandInfo> &Compiler::subcommand_table()
{
    static const std::vector<SubcommandInfo> table = {
        {
            Subcommand::t_run, "run", accepts::run,
            "compile and run them, through the JIT",
            "Compile the given source files and run the program in this process, through the JIT. "
            "Nothing is written beside your sources and no linker is involved.\n"
            "Everything after a bare '--' belongs to the program rather than to echoc, so "
            "'echoc run p.eco -- a b' passes 'a' and 'b' to the program.",
            true,
            "<sources...>",
            "the .eco files to compile and run",
            "The .eco files to compile and run. A '*' is expanded through the same expander a "
            "manifest's '#[sources:]' uses, so a pattern does not mean one thing on the command line "
            "and another in a manifest, and a file named here and not found fails the build rather "
            "than being quietly left out.\n"
            "Loose sources become the 'main' module. Give none and the program is whatever manifest "
            "this invocation points at - the one --module names, or the 'module.eco' in the working "
            "directory.",
            true
        },
        {
            Subcommand::t_build, "build", accepts::build,
            "compile and link a native executable",
            "Compile the given source files and link a native executable. Needs 'clang' on PATH for "
            "the link step.\n"
            "Each module that comes from a manifest is served from its stored object where nothing it "
            "depends on has changed. The program itself is never cached, because its unit is where "
            "the C 'main' is created.",
            true,
            "<sources...>",
            "the .eco files to compile",
            "The .eco files to compile. A '*' is expanded through the same expander a manifest's "
            "'#[sources:]' uses, so a pattern does not mean one thing on the command line and another "
            "in a manifest, and a file named here and not found fails the build rather than being "
            "quietly left out.\n"
            "Loose sources become the 'main' module. Give none and the program is whatever manifest "
            "this invocation points at - the one --module names, or the 'module.eco' in the working "
            "directory.",
            false
        },
        {
            Subcommand::t_clean, "clean", accepts::clean,
            "remove what a build produced",
            "It parses no source and runs no pass: which module directories exist is a function of "
            "the manifests and of the flags that decide where artifacts go, and reading a single .eco "
            "file to answer it would make 'clean' fail on a program that does not compile. That is "
            "also why it accepts the flags that locate a build and refuses every flag that describes "
            "one - a flag a command accepts and silently ignores is worse than one it rejects.",
            false,
            nullptr,
            nullptr,
            nullptr,
            false
        }
    };

    return table;
}

const Compiler::SubcommandInfo *Compiler::subcommand_for_word(const std::string &word)
{
    for (const SubcommandInfo &info : subcommand_table()) {
        if (word == info.name) {
            return &info;
        }
    }

    return nullptr;
}

const Compiler::SubcommandInfo &Compiler::subcommand_info(Subcommand id)
{
    for (const SubcommandInfo &info : subcommand_table()) {
        if (info.id == id) {
            return info;
        }
    }

    assert(false && "Subcommand::t_none has no info row - ask the table only about a real command");

    return subcommand_table().front();
}

const char *Compiler::retired_spelling(const std::string &word)
{
    // every spelling the overhaul removed, beside the sentence naming what replaced it. the sentences
    // say what to write rather than that something changed, because the reader wants the new word
    static const std::vector<std::pair<const char *, const char *>> retired = {
        { "-O", "'-O' is retired. Write '--optimize whole' for the merged whole-program build, which is "
                "what it did." },
        { "--no-optimize", "'--no-optimize' is retired. Write '--optimize none'." },
        { "-a", "'-a' is retired. Write '--print ast'." },
        { "--print-ast", "'--print-ast' is retired. Write '--print ast'." },
        { "-ar", "'-ar' is retired. Write '--print ast-resolved'." },
        { "--print-resolved-ast", "'--print-resolved-ast' is retired. Write '--print ast-resolved'." },
        { "--print-ir", "'--print-ir' is retired. Write '--print ir'." },
        { "-pu", "'-pu' is retired. Write '--print ir-units'." },
        { "--print-unit-ir", "'--print-unit-ir' is retired. Write '--print ir-units'." },
        { "-syt", "'-syt' is retired. Write '--print symbols'." },
        { "--print-symbol-table", "'--print-symbol-table' is retired. Write '--print symbols'." },
        { "-pi", "'-pi' is retired. Write '--print instances'." },
        { "--print-instances", "'--print-instances' is retired. Write '--print instances'." },
        { "-ec", "'-ec' is retired. Write '--explain cache'." },
        { "--explain-cache", "'--explain-cache' is retired. Write '--explain cache'." },
        { "-ep", "'-ep' is retired. Write '--explain prune'." },
        { "--explain-prune", "'--explain-prune' is retired. Write '--explain prune'." },
        { "-em", "'-em' is retired. Write '--explain memory'." },
        { "--explain-memory", "'--explain-memory' is retired. Write '--explain memory'." },
        { "-t", "'-t' is retired. Write '--explain time'." },
        { "--timings", "'--timings' is retired. Write '--explain time'." },
        { "-ta", "'-ta' is retired. Write '--track-allocations'." },
        { "--debug-info", "'--debug-info' is retired. Write '--debug-symbols', or '-g'." },
        { "--stdlib", "'--stdlib' is retired. Write '--with-stdlib'." }
    };

    for (const auto &[spelling, sentence] : retired) {
        if (word == spelling) {
            return sentence;
        }
    }

    return nullptr;
}
