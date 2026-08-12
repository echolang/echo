#ifndef TESTS_ECO_TEST_FILE_H
#define TESTS_ECO_TEST_FILE_H

#pragma once

#include "eco_check_directives.h"

#include <filesystem>
#include <string>
#include <vector>

namespace EchoTests
{
    // which dump a directive section is asserted against. one enum rather than three near copies,
    // because the only thing that differs between the three is the echoc flag that produces them
    enum class DumpKind
    {
        t_ir,

        // the same IR, but per compilation unit and without the whole-program merge - the path an
        // ordinary `echoc build` takes. **only meaningful on a `mode: build` case**: `run` folds every
        // unit into one module because the JIT can only take one, so on that path this and t_ir would
        // print the same thing
        t_unit_ir,

        t_ast,
        t_resolved_ast
    };

    // the echoc flag that emits this dump - the one place the mapping is spelled
    const char *dump_flag(DumpKind kind);

    // the section name as written in the `.test` file
    const char *dump_section_name(DumpKind kind);

    // every dump kind, for the readers that have to iterate them rather than switch on one: the
    // section-name lookup, and the message that lists the names it accepts. here beside the enum so a
    // fourth kind is added in one place - the alternative is a second list of names in the parser,
    // which is exactly what dump_section_name exists to prevent
    inline constexpr DumpKind k_dump_kinds[] = {
        DumpKind::t_ir, DumpKind::t_unit_ir, DumpKind::t_ast, DumpKind::t_resolved_ast };

    // how the program is put in front of its output. `run` is the JIT; `build` links a native binary
    // and executes it, which is the only thing that exercises LLVMCompiler::make_exec; `test` runs the
    // case's `test` blocks, which is the only thing that compiles one at all - every other mode drops them
    // before pass 1, so a case's tests are invisible unless it says so
    enum class RunMode
    {
        t_run,
        t_build,
        t_test
    };

    // what the case asserts about the exit status of the processes it spawns
    //
    // `ok` and `fail` are the two the corpus almost always wants - zero, and anything but zero. An exact
    // status is the third, and it exists because `std::env::exit($code)` made the status something a
    // program *chooses*: pinned as `fail`, `exit(3)` would pass just as well if the compiler crashed
    struct Expectation
    {
        enum class Kind
        {
            t_ok,
            t_fail,
            t_status
        };

        Kind kind = Kind::t_ok;

        // only read when kind is t_status
        int status = 0;
    };

    // did an exit status match what the case says it expects?
    bool status_matches(const Expectation &expect, int exit_code);

    // "succeed" / "fail" / "exit with 3", for the message when it did not
    std::string expectation_name(const Expectation &expect);

    struct CheckSection
    {
        DumpKind kind;
        std::vector<CheckDirective> directives;
    };

    // one parsed `<name>.test`: the settings header plus its sections
    struct EcoTestFile
    {
        // settings, all defaulted - a plain golden test writes no header at all
        std::string flags;

        // the module manifests this case builds, as paths relative to the corpus root. Relative and
        // resolved by compiler_flags below rather than written absolutely, because the tests binary's
        // working directory is not fixed - and a manifest is the one input whose *own* relative paths
        // (its sources, its dependencies) then resolve against the manifest, not against either
        std::vector<std::string> modules;

        bool stdlib = true;
        Expectation expect;
        RunMode mode = RunMode::t_run;

        // `KEY=VALUE` pairs to set in the environment of everything this case spawns, and the arguments
        // to hand the program. Both exist for `std::env`, which can otherwise only be tested against
        // whatever the machine running the suite happens to have inherited - `PATH` exists, some invented
        // key does not - which asserts almost nothing and differs between developers and CI
        //
        // whitespace separated, for `modules`' reason: the header forbids a repeated key, so a list is
        // what one line has to mean. Neither a value nor an argument may contain a space, which is no
        // loss for a corpus fixture and is what keeps these out of shell-quoting territory
        std::vector<std::string> environment;
        std::vector<std::string> arguments;

        // the lines to feed the program on standard input, **one word per line**. it exists for
        // `std::io::read_line`, which can otherwise only be tested against whatever the terminal
        // running the suite happens to be attached to - which is a hang under CI rather than an
        // assertion.
        //
        // whitespace separated for `args`' reason, and with `args`' limit: a line cannot contain a
        // space. no loss for a corpus fixture, and it is what keeps this out of shell quoting
        std::vector<std::string> stdin_lines;

        // the OUT section, mandatory. may be empty: a program that prints nothing is a legitimate
        // case, "output not asserted" is not
        std::string expected_output;

        std::vector<CheckSection> checks;

        // the flags every echoc invocation for this case carries: the stdlib switch, then whatever
        // the case asked for, spliced as text so a `flags:` line is written exactly as it would be
        // typed. trailing space, so a caller concatenates without knowing whether it is empty.
        //
        // here rather than in the runner for the reason `dump_flag` is here: what a setting *means*
        // is the format's, and a setting whose meaning is spelled in the runner is a setting the
        // README's table can silently disagree with
        std::string compiler_flags(const std::filesystem::path &corpus_root) const;

        // `KEY=VALUE KEY2=VALUE2 ` to prefix a command with, empty when the case sets none. A shell
        // assignment prefix rather than a `setenv` in the test process, because every spawn here goes
        // through `popen` and a prefix therefore reaches the JIT'd program and a linked binary by the
        // same route - and it cannot leak into the suite's own environment or into a parallel case
        //
        // trailing space when non-empty, the convention compiler_flags already follows
        std::string environment_prefix() const;

        // `printf 'alpha\nbeta\n' | ` to prefix a command with, empty when the case feeds nothing.
        //
        // a pipe rather than a temporary file for `environment_prefix`'s reason: every spawn here goes
        // through `popen`, so one spelling reaches a JIT'd program and a linked binary by the same
        // route. it goes **ahead of** the environment prefix, so the assignments still belong to the
        // program and not to the `printf`
        std::string stdin_prefix() const;

        // ` alpha beta` to append to a program invocation, empty when the case passes none.
        //
        // `run` needs a `--` ahead of them and a linked binary does not, which is the one thing the two
        // spellings differ in - so the separator is the caller's to supply and this is just the words
        std::string argument_suffix() const;
    };

    // strips a single trailing newline, the one difference the OUT golden forgives. applied to the
    // golden as it is parsed and to a captured stream before it is compared, so both sides of that
    // comparison are canonicalized by the same function
    std::string strip_trailing_newline(std::string s);

    // parses `<name>.test`, false with a `file:line: message` in `out_error` on anything it does not
    // understand: an unknown or duplicated setting key, a header line without a colon, a value
    // outside a setting's enumeration, a line that looks like a section header but is not, an
    // unknown or duplicated section name, an empty directive section, a missing OUT section.
    //
    // every one of those is an error rather than a no-op, on purpose. a `.test` that is quietly
    // half-understood is a test that quietly asserts less than its author wrote
    bool parse_eco_test_file(
        const std::filesystem::path &path, EcoTestFile &out_file, std::string &out_error);
};

#endif
