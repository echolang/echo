#ifndef COMPILEROPTIONS_H
#define COMPILEROPTIONS_H

#pragma once

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
    };
};

#endif
