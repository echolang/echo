#ifndef PHASETIMINGS_H
#define PHASETIMINGS_H

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Compiler
{
    // wall time by phase, printed by `--timings`. process-wide: parse, sema, codegen and emit live
    // on different objects, and a flag is the right knob (output sits beside the compile it describes)
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

        // against the current nesting. isolated workers fill this after they join, while a
        // ScopedPhase is still open on this thread
        void record(std::string_view phase, double milliseconds);

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

        // a function that ate `kind`, accumulated by symbol. printed after the tree
        void record_slowest(std::string_view kind, std::string_view symbol, double milliseconds);
        void record_slowest(std::string_view kind, const std::vector<std::pair<std::string, double>> &times);

        // how a symbol is printed. mangled names cannot be read back; unnamed ones print raw
        void name_symbol(std::string symbol, std::string display);

    private:
        struct Phase
        {
            std::string name;
            size_t depth = 0;
            double milliseconds = 0.0;
        };

        struct Slowest
        {
            std::string kind;
            std::string name;
            double milliseconds = 0.0;
        };

        bool _enabled = false;
        size_t _depth = 0;
        std::vector<Phase> _phases;
        std::vector<Slowest> _slowest;
        std::unordered_map<std::string, std::string> _symbol_names;
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
