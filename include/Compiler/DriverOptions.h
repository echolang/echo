#ifndef DRIVEROPTIONS_H
#define DRIVEROPTIONS_H

#pragma once

#include "Compiler/CommandLine.h"
#include "Compiler/CommandLineOption.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/TerminalCapabilities.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Compiler
{
    // stated argv after its implications. CommandLine is what was asked; this is what it means.
    // `-g` defaults `--optimize none`; `--explain memory` implies `--track-allocations`; `run` defaults
    // `--debug`, `build` `--release`; `--print ir` / `--optimize whole` merge and skip the object cache;
    // `--profile-calls` / `--profile-stdlib` imply `--profile`; `--profile` implies tracking and DWARF
    // after the `-g` rule, so a profiled build keeps its optimization
    struct DriverOptions
    {
        Subcommand subcommand = Subcommand::t_none;

        // **the only part of this struct that is hashed.** Everything reaching codegen and
        // Compiler::compute_module_keys is in here, and nothing else is - a location, a dump request or a
        // colour choice in a cache key is a cache that goes cold for nothing
        CompilerOptions options;

        // how hard, and over what. `options.codegen_opt` is the per-unit half of this and is set from it;
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

        // `test` only, and **stated only**: what a filter spells is Compiler::parse_test_filter's question
        // and what it selects is Compiler::select_tests'. Not on `options` and deliberately so - narrowing
        // which tests run changes no emitted byte of any module that is compiled, so a cache key that
        // reacted to one would go cold for a flag the objects never saw
        std::vector<std::string> filters;

        // `test` only. zero means no deadline, which is the default and the behaviour `--timeout`
        // was added to keep. milliseconds, because that is what the flag takes
        unsigned timeout_ms = 0;

        // the request, exactly as written. Compiler::TargetFacts::resolve is still the owner of what
        // they mean for `#[if:]` (including wasi→wasm32 and `--ios-device`→arm64).
        // Compiler::resolve_codegen_target is the owner of what they emit
        std::string target_os;
        std::string target_arch;

        // with `--target-os ios` on `build`: the iPhoneOS SDK and `arm64-apple-ios15.0`,
        // not the simulator. false is the default and the simulator. not a fact a
        // condition can see - Compiler::CodegenTarget is the emission half. it does
        // imply arm64 for facts when `--target-arch` was not stated, via
        // TargetFacts::resolve, so a condition cannot still see the host arch.
        // wasi is the same door: facts are wasm32 on every subcommand
        bool ios_device = false;

        // empty means "no --build-dir"; Compiler::BuildLayout::resolve is still the one arm order
        std::filesystem::path build_dir;

        // empty means vendor/ beside the entry (or the ancestor named vendor when already
        // inside vendor/<pkg> or vendor/<vendor>/<pkg>);
        // the directory `#[requires:]` names are joined onto
        std::filesystem::path package_dir;

        // `build` only, and **empty is legitimate**: a project whose manifest declares targets names its
        // own binaries. Whether an invocation that gave none needed one is settled where the manifest is
        std::filesystem::path output;

        bool no_stdlib = false;

        // skip the root's `#[hook:]`s. stated, never implied
        bool no_hooks = false;
        bool emit_stdlib_header = false;
        bool silent = false;

        // `test` only, and it is a *detail level* rather than a second `silent`: what a run reports through
        // is the terminal's answer, and how much of it is reported is this one. Compiler::TestDetail is the
        // vocabulary, settled at the one call site that builds a reporter
        bool verbose = false;

        // `install` only, and stated only: empty means no `--dir`
        // Compiler::resolve_install_dir is the arm order
        // not on `options`, because where a binary is copied changes no emitted byte
        std::filesystem::path install_dir;

        // `clean` only
        bool dry_run = false;
        bool with_stdlib = false;

        // with --print decls: include dependency and stdlib declarations. refused without -p decls
        bool decls_all = false;

        // `profile` only
        bool profile_text = false;
        std::optional<ProfileFoldKind> profile_fold;

        ColorChoice color = ColorChoice::t_auto;
        DiagnosticFormat format = DiagnosticFormat::t_auto;

        // the tail after `--`, for the JIT'd program. Empty for anything but `run`, which the parser
        // enforces rather than this struct
        std::vector<std::string> program_arguments;

        // the dump requests, as bitsets. `prints(PrintKind::t_ir)` reads at a call site the way a
        // `get<bool>("--print-ir")` did, and cannot be spelled wrong
        bool prints(PrintKind what) const;
        bool explains(ExplainKind what) const;

        // any dump: a skipped build prints nothing, so asking for one always runs
        bool prints_anything() const {
            return _prints != 0;
        }

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
