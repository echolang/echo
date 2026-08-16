#include "Compiler/CommandLineOption.h"

#include "Compiler/TerminalCapabilities.h"
#include "Compiler/TestSelection.h"

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

    // and a third, for the same reason and unlike `--link`: a filter's grammar is a tag and a word, needing
    // no working directory and no TargetFacts, so its refusal can happen at the argv word rather than after
    // the module graph has been read
    bool check_filter(const std::string &value, std::string &out_error)
    {
        Compiler::TestFilter filter;
        return Compiler::parse_test_filter(value, filter, out_error);
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
    case Subcommand::t_test:
        return accepts::test;
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
            // **no `required_by`, and that is a change rather than an omission.** The question this row
            // used to answer from argv alone - does this invocation name its output - stopped being
            // answerable there the moment a manifest could name it: a project declaring targets names its
            // binaries itself, and one declaring several has no single path to give. So the refusal moved
            // to the one place that already owns "which of these is the program", where the manifest is
            // known, and it is still raised before anything is compiled
            Opt::t_output, "output", nullptr, 'o',
            OptionArity::t_value, OptionCategory::t_inputs,
            accepts::build, 0, ExclusionGroup::t_none,
            "<file>", "",
            "where the executable is written",
            "Where to write the executable.\n"
            "  echoc build -o app src/*.eco\n"
            "If you forget it you hear about it before the compile rather than after: a build with "
            "nowhere to put what it made has wasted every second it spent.\n"
            "A project whose manifest declares targets names its own binaries, one per target, under the "
            "module's build directory - so '-o' is not needed there, and means 'put this one somewhere "
            "else'. It takes one path, so it is refused when more than one target is being built.",
            {}, nullptr
        },
        {
            Opt::t_module, "module", nullptr, 'm',
            OptionArity::t_repeated_value, OptionCategory::t_inputs,
            accepts::all, 0, ExclusionGroup::t_none,
            "<manifest>", "",
            "build a module from its manifest",
            "Build a module from its manifest. Point it at the 'module.eco' file or at the directory "
            "holding one, whichever you find easier to type:\n"
            "  echoc build -m lib -o app main.eco\n"
            "  echoc build -m lib/module.eco -o app main.eco\n"
            "You only ever name the modules you care about. The graph is resolved as a whole, so a "
            "library that depends on another pulls the other in too, once, ahead of it, and you never "
            "have to spell out a dependency yourself.\n"
            "When you do name several, the order you write them in is the order they are parsed, never "
            "the order they happen to sit in on disk.",
            {}, nullptr
        },
        {
            Opt::t_target, "target", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_inputs,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<name>", "",
            "the program to build, of the ones declared",
            "Build one of the programs a manifest declares, rather than all of them:\n"
            "  echoc build --target clock\n"
            "  echoc run --target clock\n"
            "A manifest says what its module produces with '#[target: exe { name: ..., entry: ... }]', "
            "naming one of the module's own files as the entry point. That file's top level is the "
            "program; every other file of the module contributes its declarations to all of them.\n"
            "'echoc build' with no target builds every one declared, each into its own binary under the "
            "module's build directory. Name one and only that one is built, and then '-o' may put it "
            "wherever you like. 'echoc run' takes exactly one, since one invocation runs one program.\n"
            "There is no checker on this flag, because the names belong to the manifest rather than to "
            "echoc - a name no target carries is refused with the ones that are.",
            {}, nullptr
        },
        {
            Opt::t_filter, "filter", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_inputs,
            accepts::test, 0, ExclusionGroup::t_none,
            "<spec>", "",
            "which tests to run, of the ones there are",
            "Run some of the tests rather than all of them:\n"
            "  echoc test --filter adds_up\n"
            "  echoc test --filter group:parsing --filter group:lexing\n"
            "  echoc test --filter file:src/math.eco\n"
            "A bare word is a test's name. Tag it to select by something else - 'group:', 'file:' or "
            "'module:' - which is the same tagged vocabulary '--link' reads, for the same reason: a "
            "misspelled tag is refused at the word that is wrong rather than read as a name nothing "
            "carries.\n"
            "Written more than once the filters add up, so two '--filter group:' words run both groups. A "
            "'file:' matches any path ending in what you wrote, so you need only enough of it to be "
            "unambiguous.\n"
            "A filter that matches no test at all is a refusal rather than a run of nothing - a test suite "
            "that reports success having executed nothing is the one failure it must not have.",
            {}, check_filter
        },
        {
            Opt::t_build_dir, "build-dir", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_inputs,
            accepts::all, 0, ExclusionGroup::t_none,
            "<dir>", "",
            "where build artifacts are written",
            "Keep the build's leftovers somewhere other than beside your sources:\n"
            "  echoc build --build-dir /tmp/echo-build -o app main.eco\n"
            "The whole build then lands in that one directory, one subdirectory per module. By default "
            "each module builds into 'ecobuild' beside its own manifest, or into whatever that "
            "manifest's '#[build_dir: ...]' names.\n"
            "A directory you name yourself has to carry echoc's CACHEDIR.TAG before echoc writes into it "
            "or removes anything from it. An empty one is marked for you; one that already holds files "
            "echoc did not put there is refused. That refusal is the whole reason 'echoc clean' cannot "
            "empty your source tree over a typo.",
            {}, nullptr
        },
        {
            Opt::t_package_dir, "package-dir", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_inputs,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<dir>", "",
            "directory that holds vendored packages",
            "Override the search for 'vendor/':\n"
            "  echoc build --package-dir /tmp/pkgs -o app\n"
            "A '#[requires: \"libcurl\"]' resolves to '<dir>/libcurl'. Without this flag the "
            "package directory is 'vendor/' beside the entry manifest, or the ancestor named "
            "'vendor' when the entry itself sits inside 'vendor/<pkg>' or "
            "'vendor/<vendor>/<pkg>' - so 'cd vendor/echolang/libcurl && echoc test' still "
            "finds the project's other packages. A 'vendor/' further up, past another "
            "module.eco, is not consulted.\n"
            "It is not an input to a module's cache key: paths are folded as basenames, and "
            "swapping one vendored tree for another changes the source bytes.",
            {}, nullptr
        },
        {
            Opt::t_link, "link", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_inputs,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<requirement>", "",
            "add a link requirement",
            "Tell the linker about something no module declared:\n"
            "  echoc build --link lib:m --link search:/opt/homebrew/lib -o app main.eco\n"
            "Four kinds, and the tag is not optional: 'lib:<name>' for a library, 'framework:<name>' for "
            "a Darwin framework, 'search:<dir>' for a directory to look in, 'object:<file>' for an "
            "object to link straight in.\n"
            "Most of the time you should not need any of them. A module says what it needs with "
            "'#[link:]', and whoever uses that module writes '#[depends:]' and nothing else. This flag "
            "is for the half that belongs to your machine rather than to the module, like a library "
            "installed somewhere nobody could have predicted.\n"
            "Whatever you write here is merged after every manifest's, so a declaration wins and a "
            "search path you give is consulted last.",
            {}, nullptr
        },
        {
            Opt::t_debug, "debug", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_build_mode,
            nullptr, "",
            "keep 'assert' and the runtime checks",
            "Keep 'assert' and the runtime checks the compiler emits for you, like the null check on a "
            "'ptr<T>' to 'T&' narrowing:\n"
            "  echoc build --debug -o app main.eco\n"
            "'run' does this already, 'build' does not, so this is the flag you write when you want a "
            "checked native binary.\n"
            "It decides which checks your program carries and nothing else. Optimization and debug "
            "symbols are separate axes, so a checked build can still be fully optimized. Mix them "
            "however you like.",
            {}, nullptr
        },
        {
            Opt::t_release, "release", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_build_mode,
            nullptr, "",
            "drop 'assert' and the runtime checks",
            "Drop 'assert' and the runtime checks the compiler emits for you:\n"
            "  echoc run --release bench.eco\n"
            "'build' does this already, so the interesting use is the one above: measuring something "
            "through the JIT without the checks in the way.",
            {}, nullptr
        },
        {
            Opt::t_optimize, "optimize", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<mode>", "module",
            "how hard, and over how much at once",
            "How hard the optimizer works, and how much of your program it may look at once:\n"
            "  echoc build --optimize whole -o app main.eco\n"
            "An ordinary build already optimizes every unit on its own, so reach for this when you want "
            "either less than that or more.\n"
            "It used to be two flags, '-O' and '--no-optimize'. They read like opposites and were not: "
            "one chose what is compiled together, the other whether the pipeline ran at all, so writing "
            "both was legal and meant almost nothing. It is one option with three values now.",
            {
                {
                    "none", accepts::compiling, code_of(OptimizeMode::t_none),
                    "skip the per-unit pipeline",
                    "Skip the pipeline entirely and emit the IR as codegen wrote it. Use it when you want to read "
                    "raw IR, or to find out whether the optimizer is the one miscompiling your program."
                },
                {
                    "module", accepts::compiling, code_of(OptimizeMode::t_module),
                    "optimize each unit on its own",
                    "Optimize each unit on its own, which is what you get when you say nothing at all. Also the "
                    "only mode that leaves a per-module object worth storing, so it is the only one your build "
                    "cache can help with, and that is why it is the default rather than 'whole'."
                },
                {
                    "whole", accepts::compiling, code_of(OptimizeMode::t_whole),
                    "merge every unit, then optimize",
                    "Fold every unit into one module first, then optimize the lot. The optimizer can only inline a "
                    "body it can see, so this is what gets you inlining across module boundaries, and it is the "
                    "mode to reach for when you are shipping.\n"
                    "The catch: there are no per-module objects left afterwards, so it bypasses the cache and "
                    "every build starts over. If you only need one function to stay inlinable, mark it "
                    "'#[inline]' and keep your fast builds."
                }
            },
            nullptr
        },
        {
            Opt::t_debug_symbols, "debug-symbols", nullptr, 'g',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            nullptr, "",
            "emit DWARF, so you can step through it",
            "Emit DWARF, so a debugger can step through your '.eco' files, line by line, with your own "
            "variable names:\n"
            "  echoc build -g -o app main.eco\n"
            "  lldb ./app\n"
            "On macOS echoc runs 'dsymutil' for you afterwards, because the linker leaves the DWARF "
            "behind in the objects and lldb would find nothing.\n"
            "It also turns --optimize down to none, unless you asked for a level yourself. A line table "
            "over optimized output describes a program nobody wrote, so stepping through one is a good "
            "way to lose an afternoon.\n"
            "Despite the name this is not --debug with a shorter spelling. That pair decides which "
            "checks your program carries, this decides what the object tells a debugger, and a build "
            "with no assertions that you can still step through is exactly the build you tend to want a "
            "debugger for. 'run' takes the flag and then tells you it cannot honour it: the JIT writes "
            "no object for a debugger to open.",
            {}, nullptr
        },
        {
            Opt::t_no_tbaa, "no-tbaa", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            nullptr, "",
            "emit no type-based alias metadata",
            "Emit no type-based alias metadata, which is echoc's way of telling the optimizer that two "
            "accesses cannot possibly touch the same storage.\n"
            "  echoc build --no-tbaa -o app main.eco\n"
            "Not a tuning knob: a bisection tool. A well-defined program has to give the same answer "
            "with it and without it, so if yours only works once you pass this, the flag has just told "
            "you something. Either you found a bug in echoc's access-family table, or an 'unsafe' block "
            "in your code is promising something that is not true. Either way, that comparison is what "
            "the flag is for.",
            {}, nullptr
        },
        {
            Opt::t_track_allocations, "track-allocations", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_none,
            nullptr, "",
            "count outstanding allocations",
            "Have the program keep count of how many allocations are still outstanding, so 'mem::"
            "live_allocations()' can be read from Echo:\n"
            "  echoc run --track-allocations leak_test.eco\n"
            "Without the flag that builtin is refused at compile time rather than quietly answering 0, "
            "so a leak test can never pass because somebody forgot to turn the counter on.\n"
            "It is its own flag rather than something --debug switches on, which means you can leave it "
            "out of a debug build and put it into a release one. Very handy: the build you check for "
            "leaks is then the build you actually ship.",
            {}, nullptr
        },
        {
            Opt::t_no_stdlib, "no-stdlib", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_stdlib_use,
            nullptr, "",
            "compile without the standard library",
            "Compile without the standard library:\n"
            "  echoc run --no-stdlib bare.eco\n"
            "'array', 'string', 'die', 'assert' and the 'contract::', 'mem::', 'str::', 'arr::', "
            "'hash::' and 'std::' namespaces are then simply undeclared names. Even '0 .. 10' stops "
            "meaning anything, because a range is an ordinary operator written in Echo rather than "
            "something the compiler knows about.\n"
            "The standard library is a module like any other, so nothing stops you from leaving it out. "
            "Good for a freestanding program, and a fine way to find out how little of Echo is actually "
            "built into echoc.",
            {}, nullptr
        },
        {
            Opt::t_emit_stdlib_header, "emit-stdlib-header", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_build,
            accepts::compiling, 0, ExclusionGroup::t_stdlib_use,
            nullptr, "",
            "regenerate the embedded stdlib header",
            "Regenerate 'stdlib/build/stdlib_embedded.h' from the standard library sources, which is how "
            "a downloaded echoc carries a standard library at all:\n"
            "  echoc build --emit-stdlib-header -o app main.eco\n"
            "Only interesting if you work on echoc itself. The header embeds the bytes rather than a "
            "file list, so regenerate it whenever you edit a stdlib file and not only when you add one.\n"
            "Rewriting a tracked file is opt-in rather than a side effect of every compile. Refused "
            "beside --no-stdlib, where there would be no standard library to write out.",
            {}, nullptr
        },
        {
            Opt::t_target_os, "target-os", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::all, 0, ExclusionGroup::t_none,
            "<name>", "",
            "evaluate '#[if:]' as if targeting this OS",
            "Look at the code the way another platform sees it. Every '#[if: os == ...]' condition is "
            "evaluated against the name you give here, so the other platform's regions are the ones that "
            "get parsed:\n"
            "  echoc build --target-os linux -o app main.eco\n"
            "One of 'darwin', 'linux' or 'windows'. A name outside that list is an error rather than a "
            "condition that is quietly false, because '#[if: os == darwn]' should not be a region that "
            "vanishes in silence.\n"
            "It does not cross-compile. The code is still compiled for this machine, so a foreign OS "
            "will usually fail at link. What you get is a check on another platform's branch without "
            "owning that platform, which is the only way an '#[if: os == linux]' region is ever looked "
            "at on a Mac.\n"
            "'clean' takes it too, because a manifest may hide its '#[depends:]' behind a condition: "
            "without the same flag, the graph 'clean' walks is not the graph your build produced.",
            {}, nullptr
        },
        {
            Opt::t_target_arch, "target-arch", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::all, 0, ExclusionGroup::t_none,
            "<name>", "",
            "the same for the architecture",
            "The same thing for '#[if: arch == ...]', with 'arm64' or 'x86_64' as the vocabulary:\n"
            "  echoc build --target-arch x86_64 -o app main.eco\n"
            "Same rules as --target-os, including the part where it does not cross-compile and an "
            "unknown name is an error.",
            {}, nullptr
        },
        {
            Opt::t_define, "define", nullptr, '\0',
            OptionArity::t_repeated_value, OptionCategory::t_target,
            accepts::all, 0, ExclusionGroup::t_none,
            "<name>", "",
            "declare a flag '#[if: NAME]' can test",
            "Switch on a region of your own code, guarded by '#[if: NAME]':\n"
            "  echoc run --define TRACE --define VERBOSE app.eco\n"
            "Bare names only. There is no '--define NAME=value', because naming a value is what a "
            "'const' declaration is for and echoc would rather have one way to do that than two.\n"
            "A flag you never define is false rather than an error, which is the opposite of how the os "
            "and arch vocabularies behave. It has to be: if an undefined flag were an error, "
            "'#[if: TRACE]' could never take its else arm, which is the arm you spend most of your time "
            "in.",
            {}, nullptr
        },
        {
            Opt::t_target_cpu, "target-cpu", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<name>", "",
            "which CPU to select instructions for",
            "Which CPU to optimize and select instructions for:\n"
            "  echoc build --target-cpu native -o app main.eco\n"
            "The default is the platform baseline, never this machine. 'native' is the interesting value "
            "and it is deliberately opt-in: a binary built that way may greet the machine next to yours "
            "with an illegal instruction rather than a diagnostic, and a compiler should not hand you "
            "that by default.\n"
            "It shares a word with --target-os and nothing else. That one changes what a condition sees, "
            "this one changes what comes out: every cost model in the optimizer, and instruction "
            "selection itself, sit downstream of it.",
            {}, nullptr
        },
        {
            Opt::t_target_features, "target-features", nullptr, '\0',
            OptionArity::t_value, OptionCategory::t_target,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<list>", "",
            "features to enable or disable",
            "Turn individual target features on or off, when the CPU name alone is not the answer:\n"
            "  echoc build --target-features +sve,-crc -o app main.eco\n"
            "A comma-separated list, each entry prefixed '+' or '-'. Empty unless you ask for something, "
            "and applied on top of whatever --target-cpu already implies.",
            {}, nullptr
        },
        {
            Opt::t_print, "print", nullptr, 'p',
            OptionArity::t_repeated_value, OptionCategory::t_report,
            accepts::compiling, 0, ExclusionGroup::t_none,
            "<what>", "",
            "dump what the compiler built, by layer",
            "Watch what the compiler built, one layer at a time:\n"
            "  echoc run -p ast-resolved -p ir app.eco\n"
            "Ask for two layers and you get two dumps rather than only the last one, which is what makes "
            "'the AST said this, the IR said that' a single command.\n"
            "Each dump writes a '[section]' header, so the output greps cleanly and is stable enough to "
            "assert on in a test.",
            {
                {
                    "ast", accepts::compiling, code_of(PrintKind::t_ast),
                    "the AST as parsed",
                    "The AST as parsed, before any semantic pass has touched it. This is your code, as a tree, and "
                    "nothing more."
                },
                {
                    "ast-resolved", accepts::compiling, code_of(PrintKind::t_ast_resolved),
                    "the AST after the semantic passes",
                    "The AST after the semantic passes, which is where the interesting reading is. Every deref, "
                    "drop, retain and release is a real node here that does not exist in 'ast', so this is how you "
                    "find out what echoc decided about a reference count without going near the IR."
                },
                {
                    "ir", accepts::compiling, code_of(PrintKind::t_ir),
                    "the merged whole-program LLVM IR",
                    "The LLVM IR, as one whole-program module. Worth knowing: printing it implies the merge, "
                    "because one dump can only look at one module. So what you are reading describes the "
                    "'--optimize whole' build even when you did not ask for one, and 'ir-units' is the dump that "
                    "describes an ordinary build."
                },
                {
                    "ir-units", accepts::compiling, code_of(PrintKind::t_ir_units),
                    "each unit, as the object writer gets it",
                    "Each compilation unit's IR on its own, exactly as the object writer receives it. No merge, so "
                    "this is the only dump that shows what an ordinary build really emits."
                },
                {
                    "symbols", accepts::compiling, code_of(PrintKind::t_symbols),
                    "the registered symbol table",
                    "The registered symbol table: the namespace tree holding the types, and the function registry "
                    "holding the overload sets. Read it when echoc cannot find a name you are sure you declared."
                },
                {
                    "instances", accepts::compiling, code_of(PrintKind::t_instances),
                    "generic instances and rewired calls",
                    "Which generic instances were minted, which call sites were rewired to them, and which structs "
                    "were instantiated. When a generic goes wrong, start here rather than in the IR."
                },
                {
                    "manifest", accepts::compiling, code_of(PrintKind::t_manifest),
                    "the named manifests, as JSON",
                    "Dump each named manifest as JSON and stop. Unlike every other --print value this is an "
                    "answer, not a dump of something the compiler built: it does not resolve the module graph, "
                    "so a '#[requires:]' that is not on disk yet is still readable.\n"
                    "epm is why. It must not re-implement the attribute grammar, and it reads a manifest "
                    "*before* it has fetched anything. Combine this with another --print value and it is "
                    "refused: those other values need a compile."
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
            "Ask echoc why, instead of guessing:\n"
            "  echoc build --explain cache --explain time -o app main.eco\n"
            "These are ordinary output rather than debugging leftovers, and they earn their keep: the "
            "very first run of 'time' turned up that expanding the standard library's source globs cost "
            "more than lexing all 1938 lines of it.\n"
            "Each writes a '[section]' header, so you can grep for the one you wanted.",
            {
                {
                    "cache", accepts::compiling, code_of(ExplainKind::t_cache),
                    "each module's key, and what changed",
                    "Why a module was rebuilt: its cache key, whether a stored object was found, and on a miss the "
                    "input that changed. The report to reach for when a build recompiles something you were sure "
                    "it had cached."
                },
                {
                    "prune", accepts::jitting, code_of(ExplainKind::t_prune),
                    "what the JIT prune dropped",
                    "How many function definitions the JIT threw away before running, and which ones your entry "
                    "point still reaches. 'run' and 'test' only, because only the JIT prunes - and on a 'test' "
                    "the roots it keeps are the tests being run. Written on a 'build' it is a refusal that names "
                    "them, rather than a flag quietly doing nothing."
                },
                {
                    "memory", accepts::compiling, code_of(ExplainKind::t_memory),
                    "live allocations when the program ended",
                    "How many allocations your program still held when it ended, printed by the program itself. "
                    "The odd one out here: it changes what echoc emits rather than reporting what echoc did, which "
                    "is why it implies --track-allocations."
                },
                {
                    "time", accepts::compiling, code_of(ExplainKind::t_time),
                    "where the compile spent its time",
                    "Where the compile spent its time, phase by phase, as a tree. Start here before you optimize "
                    "anything about your build."
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
            "How an error or a warning is drawn:\n"
            "  echoc build --diagnostics json -o app main.eco\n"
            "'json' is JSON Lines, one object per diagnostic and one summary object at the end, which is "
            "what an editor or a language server wants to read. 'pretty' draws the boxes and 'ascii' "
            "keeps to plain characters, for when you want to pick by hand.\n"
            "'auto' picks 'pretty' when stderr is a terminal that can draw it and 'ascii' when it is not, "
            "so a CI log, a Windows console and a golden test all end up with the same plain output and "
            "nobody has to configure anything.\n"
            "Diagnostics go to stderr whichever form you choose, so '2>' is how you capture them.",
            {}, check_diagnostics
        },
        {
            Opt::t_color, "color", "colour", '\0',
            OptionArity::t_value, OptionCategory::t_report,
            accepts::all, 0, ExclusionGroup::t_none,
            "<auto|always|never>", "auto",
            "colourise diagnostics",
            "Colourise diagnostics:\n"
            "  echoc build --color always -o app main.eco 2> build.log\n"
            "That example is not a mistake. 'always' really does put escape sequences into a file or a "
            "pipe, which is exactly what you want when something downstream renders them back out later. "
            "'never' is the other direction, and 'auto' colours a terminal and nothing else.\n"
            "NO_COLOR, CLICOLOR_FORCE and TERM=dumb are honoured, so a machine that has already said how "
            "it feels about colour does not need this flag at all.",
            {}, check_color
        },
        {
            Opt::t_silent, "silent", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_report,
            accepts::all, 0, ExclusionGroup::t_none,
            nullptr, "",
            "do not draw the progress checklist",
            "Stop drawing the checklist that rewrites itself as each module and phase finishes:\n"
            "  echoc build --silent -o app main.eco\n"
            "That is all it silences. Your diagnostics, your warnings and every --print and --explain "
            "dump were asked for by name and stay exactly where they are. A flag that goes quiet because "
            "some other flag was set is a flag you cannot rely on.\n"
            "You may not need it: the checklist is only ever drawn when stderr is a terminal that can be "
            "redrawn, so a pipe, a redirect and a CI log are silent already. There is no --progress on "
            "the other side either, because nobody wants a carriage return in a stored log.",
            {}, nullptr
        },
        {
            Opt::t_verbose, "verbose", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_report,
            accepts::test, 0, ExclusionGroup::t_none,
            nullptr, "",
            "list every test, with how long each one took",
            "List every test under the file it is written in, with how long it took:\n"
            "  echoc test --verbose\n"
            "Without it a run reports a counter and its failures, which is the right answer for a suite "
            "you expect to pass. Ask for this when you want the transcript: every test that ran, passes "
            "included, the milliseconds each one cost, and a count and a total under each file.\n"
            "It also keeps a passing test's own output, indented under its line. That is thrown away "
            "otherwise, and it is the one thing a 'dprint' in a green test has no other way of reaching "
            "you.\n"
            "It replaces the live counter rather than adding to it, so what you read on a terminal is what "
            "a pipe and a CI log record. A failure still gets its full block under the summary, where it "
            "is worth looking twice.",
            {}, nullptr
        },
        {
            Opt::t_with_stdlib, "with-stdlib", nullptr, '\0',
            OptionArity::t_flag, OptionCategory::t_removal,
            accepts::clean, 0, ExclusionGroup::t_none,
            nullptr, "",
            "also remove the standard library's store",
            "Also empty the standard library's store:\n"
            "  echoc clean --with-stdlib\n"
            "Cleaning a project leaves that store alone, because every project on this machine shares it "
            "and it is the slowest thing in any build to produce again. Ask for it when you suspect the "
            "stdlib objects themselves, and expect the next build to take a while.",
            {}, nullptr
        },
        {
            Opt::t_dry_run, "dry-run", nullptr, 'n',
            OptionArity::t_flag, OptionCategory::t_removal,
            accepts::clean, 0, ExclusionGroup::t_none,
            nullptr, "",
            "print what would be removed, remove nothing",
            "Print what would be removed and remove nothing:\n"
            "  echoc clean -n\n"
            "A good habit in general, and worth the extra command whenever you have pointed a build "
            "somewhere of your own with --build-dir or '#[build_dir:]'.",
            {}, nullptr
        },
        {
            Opt::t_help, "help", nullptr, 'h',
            OptionArity::t_flag, OptionCategory::t_general,
            accepts::all, 0, ExclusionGroup::t_none,
            nullptr, "",
            "print this page, or one option in full",
            "Print a page. Which page depends on what you put around it:\n"
            "  echoc --help\n"
            "  echoc build --help\n"
            "  echoc build --help optimize\n"
            "The last form is where the prose lives: the compact page has one line per option, and the "
            "paragraphs, the values and the gotchas are one command further in.\n"
            "The option name goes in bare, without its dashes, and a short spelling works too: 'echoc "
            "build --help g' is the --debug-symbols page.",
            {}, nullptr
        },
        {
            Opt::t_version, "version", nullptr, 'v',
            OptionArity::t_flag, OptionCategory::t_general,
            accepts::all, 0, ExclusionGroup::t_none,
            nullptr, "",
            "print the version",
            "Print the version this echoc was built as, and exit:\n"
            "  echoc --version",
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
            "compile and run through the JIT",
            "Compile your program and run it right here in this process, through the JIT. No object "
            "files, no linker, nothing written beside your sources, which makes this the quickest way to "
            "try something out:\n"
            "  echoc run app.eco\n"
            "Everything after a bare '--' belongs to your program rather than to echoc:\n"
            "  echoc run app.eco -- --verbose input.txt\n"
            "Assertions and the compiler's runtime checks are on unless you say --release, which is the "
            "opposite of what 'build' does.",
            true,
            "<sources...>",
            "the .eco files to compile and run",
            "The .eco files to compile and run. Loose files like these become the 'main' module.\n"
            "A '*' is expanded by echoc itself, through the same expander a manifest's '#[sources:]' "
            "uses, so a pattern means the same thing here as it does there. A file you name and echoc "
            "cannot find fails the build rather than being quietly left out.\n"
            "Name none at all and your program is whatever manifest this invocation points at: the one "
            "--module names, or the 'module.eco' in your working directory.",
            true
        },
        {
            Subcommand::t_build, "build", accepts::build,
            "compile and link a native executable",
            "Compile your program and link a real native executable, which needs 'clang' on your PATH "
            "for the link step:\n"
            "  echoc build -o app src/*.eco\n"
            "Assertions and the compiler's runtime checks are off unless you say --debug.\n"
            "Every module that comes from a manifest is served from its stored object for as long as "
            "nothing it depends on has changed, so a second build usually only recompiles the program "
            "itself. That part is never cached, because its unit is where the C 'main' is created.",
            true,
            "<sources...>",
            "the .eco files to compile",
            "The .eco files to compile. Loose files like these become the 'main' module.\n"
            "A '*' is expanded by echoc itself, through the same expander a manifest's '#[sources:]' "
            "uses, so a pattern means the same thing here as it does there. A file you name and echoc "
            "cannot find fails the build rather than being quietly left out.\n"
            "Name none at all and your program is whatever manifest this invocation points at: the one "
            "--module names, or the 'module.eco' in your working directory.",
            false
        },
        {
            Subcommand::t_test, "test", accepts::test,
            "compile and run this project's tests",
            "Run the 'test' blocks of the modules this invocation points at, each one in a process of its "
            "own so that a failure stops that test and nothing else:\n"
            "  echoc test\n"
            "  echoc test --filter group:parsing\n"
            "  echoc test math.eco\n"
            "Like 'run' it goes through the JIT, so it needs no linker and writes nothing beside your "
            "sources. Assertions are on unless you say --release - and a test run without them asserts "
            "nothing at all, which is worth knowing before you ask for one.\n"
            "A 'test' block is compiled only by this command. Every other invocation drops one before it "
            "is ever parsed, so tests cost a normal build nothing and cannot end up in a binary.\n"
            "Only the modules you pointed at are searched. A library you depend on keeps its own tests, "
            "and running them is a matter of pointing at it instead.",
            true,
            "<sources...>",
            "the .eco files whose tests to run",
            "The .eco files to compile and take the tests of. Loose files like these become the 'main' "
            "module, exactly as they do under 'run'.\n"
            "Name none at all and the tests are those of whatever manifest this invocation points at: the "
            "one --module names, or the 'module.eco' in your working directory.\n"
            "To run *some* of what is there, this is not the flag - use --filter, which selects by name, "
            "by group, by file or by module.",
            false
        },
        {
            Subcommand::t_clean, "clean", accepts::clean,
            "remove what a build produced",
            "Empty every build directory this project's modules write into:\n"
            "  echoc clean -n\n"
            "  echoc clean\n"
            "Do it in that order the first time. '-n' prints what would go and removes nothing.\n"
            "It parses no source and runs no pass, which is deliberate: which build directories exist "
            "follows from the manifests and from the flags that decide where artifacts go, and reading a "
            "single .eco file to work that out would make 'clean' fail on exactly the program you most "
            "want to clean, the one that does not compile.\n"
            "So it takes the flags that locate a build, like --module, --build-dir and --target-os, and "
            "refuses every flag that describes one. A flag a command accepts and silently ignores is "
            "worse than one it rejects.",
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
