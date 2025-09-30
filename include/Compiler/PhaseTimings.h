#ifndef PHASETIMINGS_H
#define PHASETIMINGS_H

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace Compiler
{
    // where a compile spent its wall time, by phase, printed by `--timings`.
    //
    // this exists because the interesting question about this compiler is not answerable from the
    // outside. `echoc run` on a program that uses the standard library takes ~3.5x as long as the same
    // program with `--no-stdlib`, and that difference is spread across the three parse passes, the
    // monomorphizer's fixpoint, IR construction, the module merge and the JIT - which of them dominates
    // decides whether it is worth caching a module's *object* or only its IR, and no amount of reasoning
    // substitutes for the number.
    //
    // **process-wide, deliberately.** The phases it has to span are owned by different objects - the
    // driver owns parsing and the semantic passes, LLVMCompiler owns codegen and the merge, Backend owns
    // emission - and threading a collector through all of them would change several signatures for a
    // diagnostic. The alternative was a getenv, which is what ECO_TRACE_MONO does; a flag is better here
    // because the output belongs beside the compile it describes.
    class PhaseTimings
    {
    public:
        static PhaseTimings &instance();

        void enable() { _enabled = true; }
        bool enabled() const { return _enabled; }

        // accumulates rather than replaces, so a phase entered once per module - parsing is - reports the
        // total rather than whichever module happened to be last. `depth` is how many phases were open when
        // this one was entered, and the report indents by it
        void record(std::string_view phase, size_t depth, double milliseconds);

        // a phase is opening: claims its row and answers how deeply it is nested. The depth is *derived* from
        // that nesting rather than written into each name, because a ScopedPhase already nests lexically -
        // spelling the indentation into ~15 call sites made the tree a whitespace convention every one of
        // them had to obey, and the first phase to move broke it silently.
        //
        // the row is claimed here rather than when the phase ends, so the report reads in the order the
        // pipeline runs. Ending is what a phase does *after* everything inside it, so ordering on that put
        // every parent below its own children
        size_t enter(std::string_view phase);
        void leave() { _depth--; }

        // the phases in the order they were first entered, so the report reads as the pipeline does
        std::string report() const;

    private:
        struct Phase
        {
            std::string name;
            size_t depth = 0;
            double milliseconds = 0.0;
        };

        bool _enabled = false;
        size_t _depth = 0;
        std::vector<Phase> _phases;
    };

    // times a phase for as long as it is in scope, so an early return cannot lose it
    class ScopedPhase
    {
    public:
        explicit ScopedPhase(std::string_view name);

        ~ScopedPhase();

        ScopedPhase(const ScopedPhase &) = delete;
        ScopedPhase &operator=(const ScopedPhase &) = delete;

    private:
        std::string _name;
        size_t _depth;
        std::chrono::steady_clock::time_point _start;
    };
};

#endif
