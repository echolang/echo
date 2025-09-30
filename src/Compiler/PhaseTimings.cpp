#include "Compiler/PhaseTimings.h"

#include <fmt/core.h>

#include <algorithm>

Compiler::PhaseTimings &Compiler::PhaseTimings::instance()
{
    static PhaseTimings timings;
    return timings;
}

namespace
{

template <typename Phases>
auto find_phase(Phases &phases, std::string_view name)
{
    return std::find_if(phases.begin(), phases.end(),
        [&name](const auto &entry) { return entry.name == name; });
}

};

size_t Compiler::PhaseTimings::enter(std::string_view phase)
{
    const size_t depth = _depth++;

    if (_enabled && find_phase(_phases, phase) == _phases.end()) {
        _phases.push_back(Phase{ std::string(phase), depth, 0.0 });
    }

    return depth;
}

void Compiler::PhaseTimings::record(std::string_view phase, size_t depth, double milliseconds)
{
    if (!_enabled) {
        return;
    }

    auto found = find_phase(_phases, phase);

    if (found == _phases.end()) {
        _phases.push_back(Phase{ std::string(phase), depth, milliseconds });
        return;
    }

    found->milliseconds += milliseconds;
}

std::string Compiler::PhaseTimings::report() const
{
    if (_phases.empty()) {
        return "";
    }

    // one column for every name at its own indentation, so the numbers still line up
    size_t width = 0;
    for (const Phase &phase : _phases) {
        width = std::max(width, phase.name.size() + phase.depth * 2);
    }

    std::string out = "[timings]\n";

    // no total: an indented phase is inside the one above it, so summing the column would double-count.
    // That is the reader's to add up - inventing a total here would be a number that is wrong in exactly
    // the interesting cases
    for (const Phase &phase : _phases) {
        const std::string indented = std::string(phase.depth * 2, ' ') + phase.name;
        out += fmt::format("  {:<{}}  {:>8.2f} ms\n", indented, width, phase.milliseconds);
    }

    return out;
}

Compiler::ScopedPhase::ScopedPhase(std::string_view name)
    : _name(name), _depth(PhaseTimings::instance().enter(name)),
      _start(std::chrono::steady_clock::now())
{}

Compiler::ScopedPhase::~ScopedPhase()
{
    PhaseTimings &timings = PhaseTimings::instance();

    timings.leave();

    if (!timings.enabled()) {
        return;
    }

    const auto elapsed = std::chrono::steady_clock::now() - _start;
    timings.record(_name, _depth, std::chrono::duration<double, std::milli>(elapsed).count());
}
