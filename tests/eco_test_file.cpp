#include "eco_test_file.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace EchoTests
{
namespace
{
    // one physical line, kept with its byte range so a section body can be sliced out of the file
    // verbatim. an OUT section is a byte golden, so it must never be rebuilt from re-joined lines -
    // that quietly loses or gains a trailing blank line
    struct LineRecord
    {
        std::string text;   // without the newline, `\r` still attached
        size_t number = 0;
        size_t begin = 0;   // offset of the first character
        size_t next = 0;    // offset just past the newline, i.e. where the next line starts
    };

    std::vector<LineRecord> split_lines(const std::string &content)
    {
        std::vector<LineRecord> lines;

        size_t pos = 0;
        size_t number = 0;

        while (pos <= content.size()) {
            const size_t newline = content.find('\n', pos);
            const size_t end = newline == std::string::npos ? content.size() : newline;

            number += 1;
            lines.push_back(LineRecord {
                content.substr(pos, end - pos), number, pos,
                newline == std::string::npos ? content.size() : newline + 1 });

            if (newline == std::string::npos) {
                break;
            }

            pos = newline + 1;
        }

        // a file ending in a newline produces a trailing empty record above; harmless, it is neither
        // a delimiter nor a setting and its bytes are already part of the previous section's slice
        return lines;
    }

    // strips a single trailing `\r`, so a file written with CRLF still has recognizable delimiters
    // and settings. never applied to section content - an OUT golden is bytes
    std::string strip_carriage_return(const std::string &line)
    {
        if (!line.empty() && line.back() == '\r') {
            return line.substr(0, line.size() - 1);
        }
        return line;
    }

    const char *const k_delimiter_prefix = "--- ";
    const char *const k_delimiter_suffix = " --->";

    // is this line exactly `--- NAME --->`, and if so what is NAME?
    //
    // an exact full-line match, deliberately, never a `rfind("---", 0) == 0` prefix test: 74 of the
    // goldens in this corpus *begin* with `---- Issue ----`, and a prefix test would truncate every
    // one of them at line 1 and leave 74 tests passing against a one-line expectation. four dashes
    // and no `--->` cannot match this
    bool match_delimiter(const std::string &line, std::string &out_name)
    {
        const std::string prefix = k_delimiter_prefix;
        const std::string suffix = k_delimiter_suffix;

        if (line.size() <= prefix.size() + suffix.size()) {
            return false;
        }

        if (line.rfind(prefix, 0) != 0 || line.compare(line.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }

        const std::string name = line.substr(prefix.size(), line.size() - prefix.size() - suffix.size());

        if (name.empty() || !(name[0] >= 'A' && name[0] <= 'Z')) {
            return false;
        }

        for (const char c : name) {
            const bool allowed = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!allowed) {
                return false;
            }
        }

        out_name = name;
        return true;
    }

    // a line that was clearly *meant* to be a delimiter but is not one. `---OUT--->`,
    // `--- out --->`, `  --- OUT --->` are all mistakes rather than content, and content is what
    // they would silently become
    //
    // the test asks for `--->` rather than merely `---`, which is exactly what keeps a diagnostic's
    // `---- Issue ----` a golden line
    bool looks_like_delimiter(const std::string &line)
    {
        const std::string trimmed = trim_whitespace(line);
        return trimmed.rfind("---", 0) == 0 && trimmed.find("--->") != std::string::npos;
    }
};

bool status_matches(Expectation expect, int exit_code)
{
    return expect == Expectation::t_ok ? exit_code == 0 : exit_code != 0;
}

const char *expectation_name(Expectation expect)
{
    return expect == Expectation::t_ok ? "succeed" : "fail";
}

std::string EcoTestFile::compiler_flags() const
{
    std::string result;

    if (!stdlib) {
        result += "--no-stdlib ";
    }

    if (!flags.empty()) {
        result += flags + " ";
    }

    return result;
}

std::string strip_trailing_newline(std::string s)
{
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }

    return s;
}

const char *dump_flag(DumpKind kind)
{
    switch (kind) {
    case DumpKind::t_ir:
        return "--print-ir";
    case DumpKind::t_ast:
        return "--print-ast";
    case DumpKind::t_resolved_ast:
        return "--print-resolved-ast";
    }

    return "";
}

const char *dump_section_name(DumpKind kind)
{
    switch (kind) {
    case DumpKind::t_ir:
        return "IR";
    case DumpKind::t_ast:
        return "AST";
    case DumpKind::t_resolved_ast:
        return "RAST";
    }

    return "";
}

namespace
{
    // a setting's value, checked against its enumeration. one helper because all three enumerated
    // settings report their mistake the same way
    bool read_enumerated(
        const std::string &origin,
        size_t line,
        const std::string &key,
        const std::string &value,
        const char *first_name,
        const char *second_name,
        bool &out_is_second,
        std::string &out_error)
    {
        if (value == first_name) {
            out_is_second = false;
            return true;
        }

        if (value == second_name) {
            out_is_second = true;
            return true;
        }

        out_error = locate(origin, line, "setting '" + key + "' must be '" + first_name + "' or '"
            + second_name + "', got: " + value);
        return false;
    }

    bool read_setting(
        const std::string &origin,
        const LineRecord &record,
        std::set<std::string> &seen,
        EcoTestFile &out_file,
        std::string &out_error)
    {
        const std::string line = trim_whitespace(record.text);

        if (line.empty() || line.rfind('#', 0) == 0) {
            return true;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            out_error = locate(origin, record.number, "expected 'key: value', got: " + line);
            return false;
        }

        const std::string key = trim_whitespace(line.substr(0, colon));
        const std::string value = trim_whitespace(line.substr(colon + 1));

        if (!seen.insert(key).second) {
            out_error = locate(origin, record.number, "setting '" + key + "' is set twice");
            return false;
        }

        if (value.empty()) {
            out_error = locate(origin, record.number, "setting '" + key + "' has no value");
            return false;
        }

        if (key == "flags") {
            out_file.flags = value;
            return true;
        }

        if (key == "stdlib") {
            bool is_off = false;
            if (!read_enumerated(origin, record.number, key, value, "on", "off", is_off, out_error)) {
                return false;
            }
            out_file.stdlib = !is_off;
            return true;
        }

        if (key == "expect") {
            bool is_fail = false;
            if (!read_enumerated(origin, record.number, key, value, "ok", "fail", is_fail, out_error)) {
                return false;
            }
            out_file.expect = is_fail ? Expectation::t_fail : Expectation::t_ok;
            return true;
        }

        if (key == "mode") {
            bool is_build = false;
            if (!read_enumerated(origin, record.number, key, value, "run", "build", is_build, out_error)) {
                return false;
            }
            out_file.mode = is_build ? RunMode::t_build : RunMode::t_run;
            return true;
        }

        out_error = locate(origin, record.number,
            "unknown setting '" + key + "', expected one of: flags, stdlib, expect, mode");
        return false;
    }
};

bool parse_eco_test_file(
    const std::filesystem::path &path, EcoTestFile &out_file, std::string &out_error)
{
    const std::string origin = path.filename().string();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out_error = origin + ": could not be opened";
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string content = buffer.str();

    const std::vector<LineRecord> lines = split_lines(content);

    // every line is classified before anything is interpreted, so a near-miss delimiter is caught
    // wherever it sits - including inside a section, where it would otherwise become content
    std::vector<size_t> delimiters;
    std::vector<std::string> names;

    for (const LineRecord &record : lines) {
        const std::string line = strip_carriage_return(record.text);

        std::string name;
        if (match_delimiter(line, name)) {
            delimiters.push_back(record.number - 1);
            names.push_back(name);
            continue;
        }

        if (looks_like_delimiter(line)) {
            out_error = locate(origin, record.number,
                "looks like a section header but is not: '" + line + "' (expected '--- NAME --->')");
            return false;
        }
    }

    if (delimiters.empty()) {
        out_error = origin + ": no sections - a test with no '--- OUT --->' asserts nothing";
        return false;
    }

    // the header is everything above the first delimiter
    std::set<std::string> seen_settings;
    for (size_t i = 0; i < delimiters.front(); i += 1) {
        if (!read_setting(origin, lines[i], seen_settings, out_file, out_error)) {
            return false;
        }
    }

    bool has_output = false;
    std::set<std::string> seen_sections;

    for (size_t i = 0; i < delimiters.size(); i += 1) {
        const LineRecord &header = lines[delimiters[i]];
        const std::string &name = names[i];

        if (!seen_sections.insert(name).second) {
            out_error = locate(origin, header.number, "section '" + name + "' appears twice");
            return false;
        }

        // sliced out of the file rather than rebuilt from lines, so an OUT golden is byte exact
        const size_t body_begin = header.next;
        const size_t body_end = i + 1 < delimiters.size() ? lines[delimiters[i + 1]].begin : content.size();
        const std::string body = content.substr(body_begin, body_end - body_begin);
        const size_t body_first_line = header.number + 1;

        if (name == "OUT") {
            // canonicalized here rather than at the comparison, so `expected_output` *is* the golden
            // for every reader. a section body ends just before the next delimiter, so it carries
            // the newline that delimiter sits behind - one trailing `\n` that belongs to the format
            // rather than to the program's output
            out_file.expected_output = strip_trailing_newline(body);
            has_output = true;
            continue;
        }

        DumpKind kind = DumpKind::t_ir;
        if (name == "IR") {
            kind = DumpKind::t_ir;
        }
        else if (name == "AST") {
            kind = DumpKind::t_ast;
        }
        else if (name == "RAST") {
            kind = DumpKind::t_resolved_ast;
        }
        else {
            out_error = locate(origin, header.number,
                "unknown section '" + name + "', expected one of: OUT, IR, AST, RAST");
            return false;
        }

        CheckSection section { kind, {} };
        if (!parse_check_directives(body, body_first_line, origin, section.directives, out_error)) {
            return false;
        }

        if (section.directives.empty()) {
            out_error = locate(origin, header.number,
                "section '" + name + "' has no directives - an empty check section asserts nothing");
            return false;
        }

        // a section of nothing but negations is the same no-op one step further in: a CHECK-NOT is
        // scoped to the region between its neighbours, so with no positive directive to bound it
        // there is no region, and every negation holds against any dump - including one the compiler
        // never produced
        const bool has_positive = std::any_of(
            section.directives.begin(), section.directives.end(),
            [](const CheckDirective &directive) { return !directive.negated; });

        if (!has_positive) {
            out_error = locate(origin, header.number,
                "section '" + name + "' has only CHECK-NOT directives - a negation needs a positive "
                "CHECK to scope it, or it asserts nothing");
            return false;
        }

        out_file.checks.push_back(std::move(section));
    }

    if (!has_output) {
        out_error = origin + ": no '--- OUT --->' section - write an empty one for a program that prints nothing";
        return false;
    }

    return true;
}
};
