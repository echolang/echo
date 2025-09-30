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
        DumpKind::t_ir, DumpKind::t_ast, DumpKind::t_resolved_ast };

    // how the program is put in front of its output. `run` is the JIT; `build` links a native binary
    // and executes it, which is the only thing that exercises LLVMCompiler::make_exec
    enum class RunMode
    {
        t_run,
        t_build
    };

    // what the case asserts about the exit status of the processes it spawns
    enum class Expectation
    {
        t_ok,
        t_fail
    };

    // did an exit status match what the case says it expects?
    bool status_matches(Expectation expect, int exit_code);

    // "succeed" / "fail", for the message when it did not
    const char *expectation_name(Expectation expect);

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
        Expectation expect = Expectation::t_ok;
        RunMode mode = RunMode::t_run;

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
