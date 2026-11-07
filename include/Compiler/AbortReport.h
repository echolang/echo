#ifndef ABORTREPORT_H
#define ABORTREPORT_H

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Compiler
{
    // the four fields `__eco_abort` prints, recovered from captured stderr.
    //
    // **the text is the ABI.** AbortCodegen::write_pieces owns the format; this is the only
    // reader. a sidecar would duplicate a stream the parent already captured
    struct AbortReport
    {
        std::string headline;
        std::string message;
        std::string file;
        int line = 0;
    };

    // last line matching `  at <file>:<digits>`. the first line is headline, or
    // `headline: message`; lines between are extra message. empty when that line is missing
    std::optional<AbortReport> parse_abort_report(std::string_view text);
};

#endif
