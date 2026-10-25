#include "Compiler/DriverOptions.h"

#include <fmt/core.h>

#include <cstddef>
#include <string>
#include <utility>

namespace
{
    unsigned int bit_for(unsigned int code)
    {
        return 1u << code;
    }
};

bool Compiler::DriverOptions::prints(PrintKind what) const
{
    return (_prints & bit_for(static_cast<unsigned int>(what))) != 0;
}

bool Compiler::DriverOptions::explains(ExplainKind what) const
{
    return (_explains & bit_for(static_cast<unsigned int>(what))) != 0;
}

bool Compiler::resolve_driver_options(
    const CommandLine &cli,
    DriverOptions &out,
    std::string &out_error
)
{
    out.subcommand = cli.subcommand;

    out.sources = cli.sources;
    out.modules = cli.list(Opt::t_module);
    out.link = cli.list(Opt::t_link);
    out.defines = cli.list(Opt::t_define);
    out.targets = cli.list(Opt::t_target);
    out.filters = cli.list(Opt::t_filter);

    if (cli.stated(Opt::t_timeout)) {
        out.timeout_ms = static_cast<unsigned>(std::stoul(cli.value(Opt::t_timeout)));
    }
    out.program_arguments = cli.program_arguments;

    out.target_os = cli.value(Opt::t_target_os);
    out.target_arch = cli.value(Opt::t_target_arch);
    out.ios_device = cli.flag(Opt::t_ios_device);
    out.build_dir = cli.value(Opt::t_build_dir);
    out.package_dir = cli.value(Opt::t_package_dir);
    out.output = cli.value(Opt::t_output);
    out.install_dir = cli.value(Opt::t_dir);

    out.no_stdlib = cli.flag(Opt::t_no_stdlib);
    out.no_hooks = cli.flag(Opt::t_no_hooks);
    out.emit_stdlib_header = cli.flag(Opt::t_emit_stdlib_header);
    out.silent = cli.flag(Opt::t_silent);
    out.verbose = cli.flag(Opt::t_verbose);
    out.dry_run = cli.flag(Opt::t_dry_run);
    out.with_stdlib = cli.flag(Opt::t_with_stdlib);

    // the two rendering answers. asked of their own parsers again rather than carried off the parse -
    // they are pure functions, and a stored answer is a second place for two readers to drift
    if (!parse_color_choice(cli.value(Opt::t_color), out.color, out_error)
        || !parse_diagnostic_format(cli.value(Opt::t_diagnostics), out.format, out_error)) {
        return false;
    }

    for (const OptionValue &value : option_for(Opt::t_print).values) {
        if (cli.prints(static_cast<PrintKind>(value.code))) {
            out._prints |= bit_for(value.code);
        }
    }

    for (const OptionValue &value : option_for(Opt::t_explain).values) {
        if (cli.explains(static_cast<ExplainKind>(value.code))) {
            out._explains |= bit_for(value.code);
        }
    }

    // the subcommand's default, then what was written over it
    // one rule, because the subcommands disagree about nothing else: `run` and `test` keep
    // the checks (`assert` is how a test says it failed), `build` and `install` drop them
    // `install` is the strongest case for the release binary: that one lands on somebody's PATH
    out.options.mode = (out.subcommand == Subcommand::t_build || out.subcommand == Subcommand::t_install)
        ? BuildMode::t_release
        : BuildMode::t_debug;

    if (cli.flag(Opt::t_debug)) {
        out.options.mode = BuildMode::t_debug;
    }
    else if (cli.flag(Opt::t_release)) {
        out.options.mode = BuildMode::t_release;
    }

    out.options.debug_info = cli.flag(Opt::t_debug_symbols);
    out.options.no_tbaa = cli.flag(Opt::t_no_tbaa);
    out.options.fixed_hash_seed = cli.flag(Opt::t_fixed_hash_seed);
    out.options.profile_calls = cli.flag(Opt::t_profile_calls);
    out.options.profile_stdlib = cli.flag(Opt::t_profile_stdlib);
    out.options.profile = cli.flag(Opt::t_profile)
        || out.options.profile_calls
        || out.options.profile_stdlib;

    // **--explain memory implies --track-allocations**, settled here rather than at the two readers: a
    // report over a counter nothing maintains does not fail, it reads zero forever - which is the answer
    // a person hoped for and so the one they would believe
    out.options.report_allocations = out.explains(ExplainKind::t_memory);
    out.options.track_allocations
        = out.options.report_allocations
        || cli.flag(Opt::t_track_allocations)
        || out.options.profile;
    out.options.check_refcounts = cli.flag(Opt::t_check_refcounts);

    // the request as written, resolved by Compiler::resolve_subtarget wherever it is needed
    out.options.target_cpu = cli.value(Opt::t_target_cpu);
    out.options.target_features = cli.value(Opt::t_target_features);

    // **-g alone means --optimize none**, and only `stated()` can tell that from a written
    // `--optimize module`. The baseline pipeline reorders, folds and inlines until stepping through the
    // line table walks a program nobody wrote and every local reads <optimized out>, which is a debug
    // build in name only - so the un-implied combination is not what anybody asking for it wanted.
    // Writing --optimize is that request said out loud, and it wins
    out.optimize = cli.optimize();

    if (out.options.debug_info && !cli.stated(Opt::t_optimize)) {
        out.optimize = OptimizeMode::t_none;
    }

    // `--profile` wants DWARF to symbolize, and must not inherit `-g`'s optimize-none.
    // applied after the `-g` rule, which keys on the written `-g`. checks stay the
    // subcommand default; `run --profile` still asserts unless `--release` was written
    if (out.options.profile) {
        out.options.debug_info = true;
    }

    out.options.no_optimize = out.optimize == OptimizeMode::t_none;

    // **three reasons, kept apart from the cache key's one.** The whole-program pipeline and a single IR
    // dump can each only look at one module, so both force the merge - but a dump changes no emitted
    // byte, and letting it reach compute_module_keys would make every module's key react to a `--print`.
    //
    // `test` still JITs, so it still merges. `run` is a cached build plus exec and follows `build`:
    // only `--optimize whole` or `--print ir` fold the program
    out.whole_program = out.subcommand == Subcommand::t_test
        || out.optimize_is_whole_program()
        || out.prints(PrintKind::t_ir);

    // `--print manifest` and `--print tools` are answers, not dumps. combining one with another `-p`
    // value would then try to compile a graph whose packages may not be on disk - and two answers are
    // two documents on one stdout, which nothing reading either could parse
    for (const auto &[answer, spelled] : {
            std::pair{ PrintKind::t_manifest, "manifest" },
            std::pair{ PrintKind::t_tools, "tools" } }) {
        if (!out.prints(answer)) {
            continue;
        }

        unsigned int others = out._prints & ~bit_for(static_cast<unsigned int>(answer));

        if (others != 0) {
            out_error = fmt::format(
                "'--print {}' is an answer, not a dump, and cannot be combined with other --print values.",
                spelled);
            return false;
        }
    }

    out.decls_all = cli.flag(Opt::t_decls_all);
    out.profile_text = cli.flag(Opt::t_text);

    if (cli.stated(Opt::t_folded)) {
        for (const OptionValue &value : option_for(Opt::t_folded).values) {
            if (cli.value(Opt::t_folded) == value.name) {
                out.profile_fold = static_cast<ProfileFoldKind>(value.code);
                break;
            }
        }
    }

    if (out.decls_all && !out.prints(PrintKind::t_decls)) {
        out_error = "'--decls-all' needs '--print decls'.";
        return false;
    }

    return true;
}
