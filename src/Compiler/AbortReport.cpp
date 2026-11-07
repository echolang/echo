#include "Compiler/AbortReport.h"

#include <charconv>
#include <string_view>

namespace
{
    bool parse_location_line(std::string_view line, Compiler::AbortReport &out)
    {
        static constexpr std::string_view prefix = "  at ";

        if (!line.starts_with(prefix)) {
            return false;
        }

        const std::string_view rest = line.substr(prefix.size());
        const size_t colon = rest.rfind(':');

        if (colon == std::string_view::npos || colon == 0) {
            return false;
        }

        const std::string_view digits = rest.substr(colon + 1);

        if (digits.empty()) {
            return false;
        }

        int line_number = 0;
        const std::from_chars_result parsed = std::from_chars(
            digits.data(),
            digits.data() + digits.size(),
            line_number);

        if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || line_number < 0) {
            return false;
        }

        out.file = std::string(rest.substr(0, colon));
        out.line = line_number;
        return true;
    }

    void fill_body(std::string_view body, Compiler::AbortReport &out)
    {
        if (body.empty()) {
            return;
        }

        const size_t newline = body.find('\n');
        const std::string_view first = newline == std::string_view::npos
            ? body
            : body.substr(0, newline);
        const std::string_view rest = newline == std::string_view::npos
            ? std::string_view()
            : body.substr(newline + 1);

        const size_t sep = first.find(": ");

        if (sep == std::string_view::npos) {
            out.headline = std::string(first);
            out.message = std::string(rest);
            return;
        }

        out.headline = std::string(first.substr(0, sep));
        out.message = std::string(first.substr(sep + 2));

        if (!rest.empty()) {
            if (!out.message.empty()) {
                out.message.push_back('\n');
            }

            out.message.append(rest);
        }
    }
};

std::optional<Compiler::AbortReport> Compiler::parse_abort_report(std::string_view text)
{
    size_t end = text.size();

    while (end > 0) {
        const size_t newline = text.rfind('\n', end - 1);
        const size_t start = newline == std::string_view::npos ? 0 : newline + 1;
        const std::string_view line = text.substr(start, end - start);

        AbortReport report;

        if (parse_location_line(line, report)) {
            const std::string_view body = start == 0
                ? std::string_view()
                : text.substr(0, start - 1);
            fill_body(body, report);
            return report;
        }

        if (newline == std::string_view::npos) {
            break;
        }

        end = newline;
    }

    return std::nullopt;
}
