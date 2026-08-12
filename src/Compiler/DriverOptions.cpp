#include "Compiler/DriverOptions.h"

#include <cstddef>

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
    out.program_arguments = cli.program_arguments;

    out.target_os = cli.value(Opt::t_target_os);
    out.target_arch = cli.value(Opt::t_target_arch);
    out.build_dir = cli.value(Opt::t_build_dir);
    out.output = cli.value(Opt::t_output);

    out.no_stdlib = cli.flag(Opt::t_no_stdlib);
    out.emit_stdlib_header = cli.flag(Opt::t_emit_stdlib_header);
    out.silent = cli.flag(Opt::t_silent);
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

    // **the subcommand's default, then what was written over it.** One rule and not two, because the two
    // subcommands disagree about nothing else: `build` hands somebody a binary and so drops the checks,
    // `run` is somebody trying something and so keeps them
    out.options.mode = out.subcommand == Subcommand::t_build
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

    // **--explain memory implies --track-allocations**, settled here rather than at the two readers: a
    // report over a counter nothing maintains does not fail, it reads zero forever - which is the answer
    // a person hoped for and so the one they would believe
    out.options.report_allocations = out.explains(ExplainKind::t_memory);
    out.options.track_allocations
        = out.options.report_allocations || cli.flag(Opt::t_track_allocations);

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

    out.options.no_optimize = out.optimize == OptimizeMode::t_none;

    // **three reasons, kept apart from the cache key's one.** The whole-program pipeline and a single IR
    // dump can each only look at one module, so both force the merge - but a dump changes no emitted
    // byte, and letting it reach compute_module_keys would make every module's key react to a `--print`.
    //
    // `run` is the third and is unconditional: the JIT holds one module in memory and emits no object, so
    // there is never a per-module artifact for it to store or reuse. It used to be a `true` written at
    // the one call site, which is the same fact with nowhere to be asked
    out.whole_program = out.subcommand == Subcommand::t_run
        || out.optimize_is_whole_program()
        || out.prints(PrintKind::t_ir);

    return true;
}
