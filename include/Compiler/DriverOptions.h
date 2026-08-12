#ifndef DRIVEROPTIONS_H
#define DRIVEROPTIONS_H

#pragma once

#include "Compiler/CommandLine.h"
#include "Compiler/CommandLineOption.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/TerminalCapabilities.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // **the sole answer to "what does this invocation mean".**
    //
    // resolved once, in main, and handed to everything below it. It replaced an argument parser threaded
    // through eighteen functions, each of which read flags back by string literal - so a renamed flag was
    // a lookup that answered false, in eighteen places, with nothing failing to compile.
    //
    // **stated versus settled is the whole of the split** with Compiler::CommandLine, and it is the same
    // rule Compiler::CompilerOptions already states about target_cpu: what was asked for is one fact and
    // what it resolved to is another. Four implications live here and nowhere else -
    //
    //   `-g` means `--optimize none` unless --optimize was stated
    //   `--explain memory` implies `--track-allocations`
    //   `run` defaults to --debug and `build` to --release
    //   `--print ir` or `--optimize whole` means one merged module and no object cache
    //
    // - and three of those used to sit in resolve_options while the fourth sat in
    // wants_whole_program_module, which is two owners for one question
    struct DriverOptions
    {
        Subcommand subcommand = Subcommand::t_none;

        // **the only part of this struct that is hashed.** Everything reaching codegen and
        // Compiler::compute_module_keys is in here, and nothing else is - a location, a dump request or a
        // colour choice in a cache key is a cache that goes cold for nothing
        CompilerOptions options;

        // how hard, and over what. `options.no_optimize` is the per-unit half of this and is set from it;
        // `whole_program` below is the driver half
        OptimizeMode optimize = OptimizeMode::t_module;

        // is every unit folded into one module, with no per-module object left to store.
        //
        // **two reasons and they stay two.** `--optimize whole` is one; `--print ir` is the other, because
        // a single IR dump can only look at one module. They are deliberately *not* the same answer as the
        // bool compute_module_keys folds into a key - see optimize_is_whole_program()
        bool whole_program = false;

        // what compute_module_keys is passed. **`--print ir` is not in it**: a dump changes no emitted
        // byte, so a key that reacted to one would go cold for a dump - and worse, would mean something
        // different from what it means today with nothing detecting the change
        bool optimize_is_whole_program() const {
            return optimize == OptimizeMode::t_whole;
        }

        std::vector<std::string> sources;
        std::vector<std::string> modules;
        std::vector<std::string> link;
        std::vector<std::string> defines;

        // the targets named on the command line, and **stated only**: which targets exist is the
        // manifest's answer, so nothing here has been checked against one. Empty means "whatever the
        // manifest declares", which for a manifest declaring none is the one program it always was
        std::vector<std::string> targets;

        // the request, exactly as written. Compiler::TargetFacts::resolve is still the owner of what they
        // mean, asked in run_front_end and in main_clean as it is today
        std::string target_os;
        std::string target_arch;

        // empty means "no --build-dir"; Compiler::BuildLayout::resolve is still the one arm order
        std::filesystem::path build_dir;

        // `build` only, and **empty is legitimate**: a project whose manifest declares targets names its
        // own binaries. Whether an invocation that gave none needed one is settled where the manifest is
        std::filesystem::path output;

        bool no_stdlib = false;
        bool emit_stdlib_header = false;
        bool silent = false;

        // `clean` only
        bool dry_run = false;
        bool with_stdlib = false;

        ColorChoice color = ColorChoice::t_auto;
        DiagnosticFormat format = DiagnosticFormat::t_auto;

        // the tail after `--`, for the JIT'd program. Empty for anything but `run`, which the parser
        // enforces rather than this struct
        std::vector<std::string> program_arguments;

        // the dump requests, as bitsets. `prints(PrintKind::t_ir)` reads at a call site the way a
        // `get<bool>("--print-ir")` did, and cannot be spelled wrong
        bool prints(PrintKind what) const;
        bool explains(ExplainKind what) const;

    private:

        unsigned int _prints = 0;
        unsigned int _explains = 0;

        friend bool resolve_driver_options(
            const CommandLine &cli,
            DriverOptions &out,
            std::string &out_error);
    };

    // false with a sentence, the shape every other resolver in this compiler has.
    //
    // it can fail only on a value whose acceptance somebody else owns - `--color`, `--diagnostics` - and
    // it asks those parsers again rather than carrying what the table's `check` already learned. **A pure
    // function asked twice and stored nowhere**, which is the rule Compiler::resolve_subtarget already
    // follows for the same reason: a stored answer is a second place for two readers to drift
    bool resolve_driver_options(
        const CommandLine &cli,
        DriverOptions &out,
        std::string &out_error);
};

#endif
