#ifndef COMPILEROPTIONS_H
#define COMPILEROPTIONS_H

#pragma once

#include <string>

namespace Compiler
{
    // how much the compiler owes the running program in checks it could skip
    //
    // this is a property of *the program being compiled*, not of how echoc itself was built. the
    // distinction is the whole reason the type exists: the null-narrowing check used to be gated on
    // the host compiler's NDEBUG, which meant a release build of a user's program still carried
    // every check as long as echoc had been built with assertions on
    enum class BuildMode
    {
        t_debug,
        t_release,
    };

    struct CompilerOptions
    {
        BuildMode mode = BuildMode::t_debug;

        // does the program keep count of how many allocations are outstanding?
        //
        // its own option rather than a consequence of the build mode, and that is the point: a debug
        // build is about the checks the program owes *itself*, and this is bookkeeping a person asked
        // for. Tying it to `assertions_enabled()` would have made the emitted allocation call spell
        // itself differently in the two modes, which quietly weakens every IR golden written against
        // the wrong one - and would have left `mode: build` cases unable to be leak-checked at all
        bool track_allocations = false;

        // does `main` print the `[memory]` section on its way out? implies the above, resolved once
        // where the options are built - a report over a counter nothing maintains reads zero forever
        bool report_allocations = false;

        // **turns off the per-unit baseline pipeline** that Backend::optimize_unit runs on every ordinary
        // build. for reading raw IR, for bisecting a miscompile against the optimizer, and for the two
        // goldens that pin unoptimized output on purpose. it is not the inverse of `-O`, which is a
        // *different* thing: that one merges every unit and runs O3 over the result
        //
        // both emitters read it from here rather than from the flag - Backend::print_unit_ir dumps what
        // LLVMCompiler::emit_objects is about to write, so a second reader is a way for the two to disagree
        bool no_optimize = false;

        // **does the emitted object carry DWARF**, so a debugger can name a source line, a function and
        // a local.
        //
        // a third axis and not a consequence of the build mode, for track_allocations' reason: `--debug`
        // is about the checks the *program* owes itself, and this is what the *object* tells a debugger.
        // folding them would make an assertion-free build undebuggable by construction, which is exactly
        // the build a person reaches for a debugger over
        //
        // it does settle one thing at the CLI rather than here - see resolve_options - because the O2
        // pipeline Backend::optimize_unit runs on every ordinary build reorders and folds until a line
        // table describes a program nobody wrote. `-g` alone therefore also sets no_optimize, and `-O`
        // said out loud overrides it
        bool debug_info = false;

        // **turns off type-based alias metadata**, for differential testing and for bisecting a
        // miscompile against the aliasing model rather than against the optimizer.
        //
        // it is not a tuning knob: a *defined* program must produce the same answer with it and
        // without it, and any program where the two disagree has either found a bug in
        // AST::access_family_of or made a false `unsafe` promotion. that equivalence is the whole
        // reason the flag exists, so the e2e corpus runs its adversarial cases both ways
        bool no_tbaa = false;

        // **what was asked for, never what it resolved to.**
        //
        // `--target-cpu` and `--target-features`, both empty meaning "the platform baseline for this
        // triple". Compiler::resolve_subtarget is what turns them into the CPU and feature string the
        // backend gets, and it is a pure function - so Backend::init_target and
        // Compiler::compute_module_keys reach the identical answer without either of them storing it.
        //
        // storing the *resolved* pair here instead would make a default-constructed CompilerOptions -
        // every unit test, EchoTests::tests_compiler_options() - mean something different from an
        // invocation with no flag, which is the shape of gap that reads right until a golden moves
        std::string target_cpu;
        std::string target_features;

        // one predicate, because more than one emitter asks it - the `assert` builtin and the
        // `ptr<T>` -> `T&` narrowing today, whatever check comes next tomorrow. never compare the
        // enum at a call site, or the next check added answers the question its own way
        bool assertions_enabled() const {
            return mode == BuildMode::t_debug;
        }

        // the same rule for the two above: read them through here. `tracking_allocations()` is asked by
        // the allocation seam *and* by the pass that refuses `mem::live_allocations()` without it, so a
        // call site comparing the field is a second answer to one question
        bool tracking_allocations() const {
            return track_allocations;
        }

        bool reporting_allocations() const {
            return report_allocations;
        }

        // and the same rule again. four emitters will ask - the unit's compile unit, the subprogram, the
        // location seam and the local declares - and Compiler::compute_module_keys is a fifth, because an
        // object carrying DWARF is not the object beside it that does not
        bool emitting_debug_info() const {
            return debug_info;
        }
    };
};

#endif
