#include "Compiler/LLVM/Codegen/FunctionTimeTrace.h"
#include "Compiler/PhaseTimings.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <optional>
#include <unordered_map>

namespace
{

struct PassEvent
{
    double start_us = 0.0;
    double duration_us = 0.0;
    std::string detail;
};

bool is_legacy_event(llvm::StringRef name)
{
    return name == "OptModule" || name == "RunPass";
}

// an SCC of one function is just that function; a real cycle stays as it is
std::string unit_of(std::string detail)
{
    if (detail.size() > 2 && detail.front() == '(' && detail.back() == ')'
        && detail.find(", ") == std::string::npos) {
        return detail.substr(1, detail.size() - 2);
    }

    return detail;
}

// events nest, so each one is credited only the time its children do not cover
void credit_self_times(std::vector<PassEvent> &events, std::unordered_map<std::string, double> &totals)
{
    std::sort(events.begin(), events.end(), [](const PassEvent &a, const PassEvent &b) {
        if (a.start_us != b.start_us) {
            return a.start_us < b.start_us;
        }

        return a.duration_us > b.duration_us;
    });

    std::vector<double> self(events.size(), 0.0);
    std::vector<size_t> open;

    for (size_t i = 0; i < events.size(); i++) {
        const PassEvent &event = events[i];

        while (!open.empty()) {
            const PassEvent &top = events[open.back()];

            if (event.start_us < top.start_us + top.duration_us) {
                break;
            }

            open.pop_back();
        }

        self[i] = event.duration_us;

        if (!open.empty()) {
            self[open.back()] -= event.duration_us;
        }

        open.push_back(i);
    }

    for (size_t i = 0; i < events.size(); i++) {
        // `[module]` is a module pass: it belongs to no function
        if (events[i].detail == "[module]" || self[i] <= 0.0) {
            continue;
        }

        totals[unit_of(events[i].detail)] += self[i] / 1000.0;
    }
}

void ingest_function_times(std::vector<std::pair<std::string, double>> &out)
{
    if (!llvm::timeTraceProfilerEnabled()) {
        return;
    }

    llvm::SmallString<0> json;
    llvm::raw_svector_ostream stream(json);
    llvm::timeTraceProfilerWrite(stream);

    llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(json.str());

    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        return;
    }

    const llvm::json::Object *root = parsed->getAsObject();
    const llvm::json::Array *events = root != nullptr ? root->getArray("traceEvents") : nullptr;

    if (events == nullptr) {
        return;
    }

    std::vector<PassEvent> passes;
    std::unordered_map<std::string, double> totals;

    for (const llvm::json::Value &event : *events) {
        const llvm::json::Object *row = event.getAsObject();

        if (row == nullptr || row->getString("ph") != "X") {
            continue;
        }

        const std::optional<llvm::StringRef> name = row->getString("name");
        const std::optional<double> start = row->getNumber("ts");
        const std::optional<double> microseconds = row->getNumber("dur");
        const llvm::json::Object *args = row->getObject("args");
        const std::optional<llvm::StringRef> detail
            = args != nullptr ? args->getString("detail") : std::nullopt;

        if (!name.has_value() || !start.has_value() || !microseconds.has_value()
            || !detail.has_value() || detail->empty()) {
            continue;
        }

        if (name.value() == "OptFunction") {
            totals[detail->str()] += microseconds.value() / 1000.0;
            continue;
        }

        if (is_legacy_event(name.value())) {
            continue;
        }

        passes.push_back(PassEvent{ start.value(), microseconds.value(), detail->str() });
    }

    credit_self_times(passes, totals);
    out.insert(out.end(), totals.begin(), totals.end());
}

};

Compiler::LLVM::FunctionTimeTrace::FunctionTimeTrace(std::vector<std::pair<std::string, double>> *out)
    : _out(out)
{
    if (_out == nullptr || llvm::timeTraceProfilerEnabled()) {
        return;
    }

    // 500 us: far below the 50 ms a row needs, and the trace stays small
    llvm::timeTraceProfilerInitialize(500, "echoc");
    _live = true;
}

Compiler::LLVM::FunctionTimeTrace::FunctionTimeTrace(std::string_view kind)
    : FunctionTimeTrace(Compiler::PhaseTimings::instance().enabled() ? &_recorded : nullptr)
{
    _kind = kind;
}

Compiler::LLVM::FunctionTimeTrace::~FunctionTimeTrace()
{
    if (!_live) {
        return;
    }

    ingest_function_times(*_out);
    llvm::timeTraceProfilerCleanup();

    if (!_kind.empty()) {
        Compiler::PhaseTimings::instance().record_slowest(_kind, _recorded);
    }
}
