#include "eco_check_directives.h"

#include "Compiler/TargetFacts.h"

#include <sstream>
#include <string_view>

namespace EchoTests
{
namespace
{
    std::string ascii_lower(std::string text)
    {
        for (char &c : text) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }

        return text;
    }

    std::string known_os_list()
    {
        std::string listed;

        for (const std::string &name : Compiler::TargetFacts::known_operating_systems()) {
            if (!listed.empty()) {
                listed += ", ";
            }

            listed += name;
        }

        return listed;
    }

    // `CHECK:` / `CHECK-NOT:` / `CHECK-<os>:` / `CHECK-NOT-<os>:`. CHECK-NOT has to be tried
    // before CHECK, or `CHECK-NOT:` is read as a positive whose text starts with "-NOT:"
    bool parse_directive_prefix(
        const std::string &line,
        bool &out_negated,
        std::string &out_host_os,
        size_t &out_text_at,
        std::string &out_error
    )
    {
        out_negated = false;
        out_host_os.clear();

        size_t pos = 0;

        if (line.starts_with("CHECK-NOT")) {
            out_negated = true;
            pos = 9;
        }
        else if (line.starts_with("CHECK")) {
            pos = 5;
        }
        else {
            out_error = "expected 'CHECK:' or 'CHECK-NOT:', or a host-gated form "
                "CHECK-<os>: / CHECK-NOT-<os>: where <os> is one of "
                + known_os_list() + ", got: " + line;
            return false;
        }

        if (pos < line.size() && line[pos] == '-') {
            const size_t colon = line.find(':', pos);

            if (colon == std::string::npos) {
                out_error = "expected ':' after the host name, got: " + line;
                return false;
            }

            const std::string os_token = ascii_lower(line.substr(pos + 1, colon - pos - 1));

            if (!Compiler::TargetFacts::is_known_operating_system(os_token)) {
                out_error = "unknown host '" + os_token + "' in CHECK directive, expected one of: "
                    + known_os_list();
                return false;
            }

            out_host_os = os_token;
            pos = colon;
        }

        if (pos >= line.size() || line[pos] != ':') {
            out_error = "expected 'CHECK:' or 'CHECK-NOT:', or a host-gated form "
                "CHECK-<os>: / CHECK-NOT-<os>: where <os> is one of "
                + known_os_list() + ", got: " + line;
            return false;
        }

        out_text_at = pos + 1;
        return true;
    }
};

std::string trim_whitespace(const std::string &s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::string locate(const std::string &origin, size_t line, const std::string &message)
{
    return origin + ":" + std::to_string(line) + ": " + message;
}

bool parse_check_directives(
    const std::string &body,
    size_t first_line,
    const std::string &origin,
    std::vector<CheckDirective> &out_directives,
    std::string &out_error
)
{
    std::istringstream in(body);

    std::string raw;
    size_t line_number = first_line - 1;

    while (std::getline(in, raw)) {
        line_number += 1;
        const std::string line = trim_whitespace(raw);

        if (line.empty() || line.rfind('#', 0) == 0 || line.rfind("//", 0) == 0) {
            continue;
        }

        bool negated = false;
        std::string host_os;
        size_t text_at = 0;
        std::string prefix_error;

        if (!parse_directive_prefix(line, negated, host_os, text_at, prefix_error)) {
            out_error = locate(origin, line_number, prefix_error);
            return false;
        }

        out_directives.push_back(CheckDirective {
            negated, trim_whitespace(line.substr(text_at)), line_number, std::move(host_os) });

        if (out_directives.back().text.empty()) {
            out_error = locate(origin, line_number, "directive has no text to match");
            return false;
        }
    }

    return true;
}

std::string apply_check_directives(
    const std::vector<CheckDirective> &directives,
    const std::string &haystack,
    const std::string &host_os
)
{
    size_t cursor = 0;
    std::vector<const CheckDirective *> pending_negations;

    auto applies = [&](const CheckDirective &directive) {
        return directive.host_os.empty() || directive.host_os == host_os;
    };

    auto check_negations = [&](size_t region_end) -> std::string {
        for (const auto *negated : pending_negations) {
            const size_t found = haystack.find(negated->text, cursor);

            if (found != std::string::npos && found < region_end) {
                return "CHECK-NOT on line " + std::to_string(negated->line)
                    + " matched, but must not: " + negated->text;
            }
        }
        pending_negations.clear();
        return "";
    };

    for (const auto &directive : directives) {
        if (!applies(directive)) {
            continue;
        }

        if (directive.negated) {
            pending_negations.push_back(&directive);
            continue;
        }

        const size_t found = haystack.find(directive.text, cursor);

        if (found == std::string::npos) {
            return "CHECK on line " + std::to_string(directive.line)
                + " never matched (searching from offset " + std::to_string(cursor) + "): "
                + directive.text;
        }

        if (std::string failure = check_negations(found); !failure.empty()) {
            return failure;
        }

        cursor = found + directive.text.size();
    }

    // trailing CHECK-NOTs are scoped to everything after the last positive match
    return check_negations(haystack.size());
}

std::string apply_check_directives(
    const std::vector<CheckDirective> &directives, const std::string &haystack)
{
    return apply_check_directives(
        directives, haystack, Compiler::TargetFacts::host().operating_system);
}
};
