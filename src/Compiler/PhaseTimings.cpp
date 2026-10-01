#include "Compiler/PhaseTimings.h"

#include <fmt/core.h>

#include <algorithm>
#include <map>
#include <string_view>

std::string Compiler::display_function_name(std::string_view mangled)
{
    std::string_view name = mangled;
    const size_t z = name.find('Z');

    if (z != std::string_view::npos) {
        name = name.substr(0, z);
    }

    if (!name.empty() && name.front() == '_') {
        name.remove_prefix(1);
    }

    const size_t owner = name.find('M');

    if (owner == std::string_view::npos) {
        return std::string(name);
    }

    const std::string_view rest = name.substr(owner + 1);
    const size_t split = rest.rfind('_');

    if (split == std::string_view::npos) {
        return std::string(rest);
    }

    return std::string(rest.substr(0, split)) + "::" + std::string(rest.substr(split + 1));
}

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

void Compiler::PhaseTimings::record(std::string_view phase, double milliseconds)
{
    record(phase, _depth, milliseconds);
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

void Compiler::PhaseTimings::record_slowest(
    std::string_view kind,
    std::string_view name,
    double milliseconds
)
{
    if (!_enabled || name.empty()) {
        return;
    }

    for (Slowest &entry : _slowest) {
        if (entry.kind == kind && entry.name == name) {
            entry.milliseconds += milliseconds;
            return;
        }
    }

    _slowest.push_back(Slowest{ std::string(kind), std::string(name), milliseconds });
}

namespace
{

// listed only when a function was actually slow. 50 ms is long enough that a name is worth reading;
// six names is the most a person will look at; the rest is one line, and only when it is itself
// another quarter-second, so a long tail of 2 ms bodies does not print as "+ 400 more, 40.00 ms"
constexpr double k_slowest_floor_ms = 50.0;
constexpr size_t k_slowest_listed = 6;
constexpr double k_slowest_rest_floor_ms = 250.0;

struct SlowestRow
{
    std::string name;
    double milliseconds = 0.0;
};

void append_slowest(
    std::string &out,
    size_t width,
    std::string_view kind,
    std::vector<SlowestRow> entries
)
{
    entries.erase(
        std::remove_if(
            entries.begin(), entries.end(),
            [](const SlowestRow &entry) { return entry.milliseconds < k_slowest_floor_ms; }),
        entries.end());

    if (entries.empty()) {
        return;
    }

    std::sort(entries.begin(), entries.end(), [](const SlowestRow &a, const SlowestRow &b) {
        return a.milliseconds > b.milliseconds;
    });

    out += fmt::format("  slowest ({})\n", kind);

    const size_t listed = std::min(entries.size(), k_slowest_listed);

    for (size_t i = 0; i < listed; i++) {
        const std::string indented = "  " + entries[i].name;
        out += fmt::format("  {:<{}}  {:>8.2f} ms\n", indented, width, entries[i].milliseconds);
    }

    if (entries.size() <= listed) {
        return;
    }

    double rest = 0.0;

    for (size_t i = listed; i < entries.size(); i++) {
        rest += entries[i].milliseconds;
    }

    if (rest < k_slowest_rest_floor_ms) {
        return;
    }

    const std::string more = fmt::format("  + {} more", entries.size() - listed);
    out += fmt::format("  {:<{}}  {:>8.2f} ms\n", more, width, rest);
}

};

std::string Compiler::PhaseTimings::report() const
{
    if (_phases.empty() && _slowest.empty()) {
        return "";
    }

    // one column for every name at its own indentation, so the numbers still line up
    size_t width = 0;
    for (const Phase &phase : _phases) {
        width = std::max(width, phase.name.size() + phase.depth * 2);
    }

    for (const Slowest &entry : _slowest) {
        width = std::max(width, std::string("slowest (").size() + entry.kind.size() + 1);
        width = std::max(width, entry.name.size() + 2);
    }

    std::string out = "[timings]\n";

    // no total: an indented phase is inside the one above it, so summing the column would double-count.
    // That is the reader's to add up - inventing a total here would be a number that is wrong in exactly
    // the interesting cases
    for (const Phase &phase : _phases) {
        const std::string indented = std::string(phase.depth * 2, ' ') + phase.name;
        out += fmt::format("  {:<{}}  {:>8.2f} ms\n", indented, width, phase.milliseconds);
    }

    std::map<std::string, std::vector<SlowestRow>> by_kind;

    for (const Slowest &entry : _slowest) {
        by_kind[entry.kind].push_back(SlowestRow{ entry.name, entry.milliseconds });
    }

    for (auto &[kind, entries] : by_kind) {
        append_slowest(out, width, kind, std::move(entries));
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
