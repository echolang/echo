#include "Compiler/TestReportWriter.h"

#include "Compiler/ProgressReporter.h"
#include "Compiler/TestSelection.h"
#include "eco.h"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace
{
    const char *status_of(const Compiler::TestResult &result)
    {
        switch (result.outcome) {
        case Compiler::TestOutcome::t_failed:
            return "fail";
        case Compiler::TestOutcome::t_signalled:
            return "signal";
        case Compiler::TestOutcome::t_timed_out:
            return "timeout";
        case Compiler::TestOutcome::t_passed:
            return "pass";
        }

        return "pass";
    }
};

Compiler::TestReportWriter::TestReportWriter(std::ostream &out) :
    _out(out)
{
}

void Compiler::TestReportWriter::write_line(const std::string &line)
{
    _out << line << "\n";
    _out.flush();
}

void Compiler::TestReportWriter::begin(size_t total)
{
    _total = total;
    _started = std::chrono::steady_clock::now();

    nlohmann::ordered_json run;
    run["v"] = 2;
    run["type"] = "run";
    run["version"] = ECO_VERSION_STRING;
    run["selected"] = total;
    write_line(run.dump());
}

void Compiler::TestReportWriter::result(const TestResult &result)
{
    if (result.failed()) {
        _failed += 1;
    }

    nlohmann::ordered_json row;
    row["v"] = 2;
    row["type"] = "test";
    row["id"] = test_display_name(result.test);
    row["module"] = result.test.module;
    row["file"] = result.test.file.string();
    row["name"] = result.test.name;
    row["group"] = result.test.group;
    row["status"] = status_of(result);
    row["exit"] = result.status;
    row["signal"] = result.signal;
    row["ms"] = result.milliseconds;
    row["output"] = result.output;

    if (result.failure) {
        nlohmann::ordered_json failure;
        failure["headline"] = result.failure->headline;
        failure["message"] = result.failure->message;
        failure["file"] = result.failure->file;
        failure["line"] = result.failure->line;
        row["failure"] = failure;
    }

    write_line(row.dump());
}

void Compiler::TestReportWriter::finish()
{
    nlohmann::ordered_json end;
    end["v"] = 2;
    end["type"] = "end";
    end["selected"] = _total;
    end["failed"] = _failed;
    end["ms"] = progress_elapsed_ms(_started);
    write_line(end.dump());
}
