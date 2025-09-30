#include "eco_test_file.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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

    constexpr std::string_view k_delimiter_prefix = "--- ";
    constexpr std::string_view k_delimiter_suffix = " --->";

    // is this line exactly `--- NAME --->`, and if so what is NAME?
    //
    // an exact full-line match, deliberately, never a `rfind("---", 0) == 0` prefix test: 74 of the
    // goldens in this corpus *begin* with `---- Issue ----`, and a prefix test would truncate every
    // one of them at line 1 and leave 74 tests passing against a one-line expectation. four dashes
    // and no `--->` cannot match this
    bool match_delimiter(const std::string &line, std::string &out_name)
    {
        if (line.size() <= k_delimiter_prefix.size() + k_delimiter_suffix.size()) {
            return false;
        }

        if (!line.starts_with(k_delimiter_prefix) || !line.ends_with(k_delimiter_suffix)) {
            return false;
        }

        const std::string name = line.substr(
            k_delimiter_prefix.size(),
            line.size() - k_delimiter_prefix.size() - k_delimiter_suffix.size());

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

std::string EcoTestFile::compiler_flags(const std::filesystem::path &corpus_root) const
{
    std::string result;

    if (!stdlib) {
        result += "--no-stdlib ";
    }

    // one `-m` per manifest, resolved against the corpus root so the case reads as a path relative to
    // the test file rather than to whatever directory the tests binary was launched from
    for (const std::string &manifest : modules) {
        result += "-m \"" + (corpus_root / manifest).string() + "\" ";
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

    // no fallback: a kind with no arm here would produce an echoc invocation with no dump flag at
    // all, so the directives would run against the program's own output and hold or fail for reasons
    // that have nothing to do with the case. everything this format does not understand is an error
    assert(false && "unhandled DumpKind - add its echoc flag");
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

    // and no fallback here either: an unnamed kind is a section header nothing matches, and the
    // parser's "expected one of" message enumerates this very function
    assert(false && "unhandled DumpKind - add its section name");
    return "";
}

namespace
{
    // the accepted setting keys, so the dispatch below and the message that rejects an unknown one
    // enumerate the same list - the same reason dump_section_name is the only spelling of a section
    // name. a key added to the dispatch and forgotten here would leave the error telling an author
    // that a valid key is invalid
    constexpr std::string_view k_setting_keys[] = { "flags", "modules", "stdlib", "expect", "mode" };

    // a setting's value, checked against its enumeration and written straight into the field it
    // settles. one helper because all three enumerated settings report their mistake the same way,
    // and templated on the destination so that each spelling sits beside the value it means - reading
    // into a `bool is_second` left every caller inverting or mapping the answer afterwards, and one
    // of the three inverted it
    //
    // the spellings arrive as a list rather than as two parameters, so the helper carries no opinion
    // about how many a setting has: a three-valued `mode` would otherwise need a second copy of it,
    // message included
    template <typename T>
    bool read_enumerated(
        const std::string &origin,
        const LineRecord &record,
        const std::string &key,
        const std::string &value,
        std::initializer_list<std::pair<const char *, T>> options,
        T &out_value,
        std::string &out_error)
    {
        std::string spellings;

        for (const auto &option : options) {
            if (value == option.first) {
                out_value = option.second;
                return true;
            }

            spellings += (spellings.empty() ? "'" : "' or '") + std::string(option.first);
        }

        out_error = locate(origin, record.number,
            "setting '" + key + "' must be " + spellings + "', got: " + value);
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

        // whitespace separated, because the header forbids a repeated key and a list is what this setting
        // means. A path holding a space is not supported and does not need to be: these are corpus fixtures
        if (key == "modules") {
            std::istringstream stream(value);
            std::string entry;
            while (stream >> entry) {
                out_file.modules.push_back(entry);
            }
            return true;
        }

        if (key == "stdlib") {
            return read_enumerated<bool>(origin, record, key, value,
                { { "on", true }, { "off", false } }, out_file.stdlib, out_error);
        }

        if (key == "expect") {
            return read_enumerated<Expectation>(origin, record, key, value,
                { { "ok", Expectation::t_ok }, { "fail", Expectation::t_fail } },
                out_file.expect, out_error);
        }

        if (key == "mode") {
            return read_enumerated<RunMode>(origin, record, key, value,
                { { "run", RunMode::t_run }, { "build", RunMode::t_build } },
                out_file.mode, out_error);
        }

        std::string known;
        for (const std::string_view accepted : k_setting_keys) {
            known += (known.empty() ? "" : ", ") + std::string(accepted);
        }

        out_error = locate(origin, record.number,
            "unknown setting '" + key + "', expected one of: " + known);
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
    //
    // the line and its name travel together rather than as two index-synced vectors: the byte range
    // of the *next* header is what bounds this section's body, and that is the only thing about a
    // delimiter the loop below needs to look up
    struct SectionHeader
    {
        const LineRecord *line;
        std::string name;
    };

    std::vector<SectionHeader> headers;

    for (const LineRecord &record : lines) {
        const std::string line = strip_carriage_return(record.text);

        std::string name;
        if (match_delimiter(line, name)) {
            headers.push_back(SectionHeader { &record, std::move(name) });
            continue;
        }

        if (looks_like_delimiter(line)) {
            out_error = locate(origin, record.number,
                "looks like a section header but is not: '" + line + "' (expected '--- NAME --->')");
            return false;
        }
    }

    if (headers.empty()) {
        out_error = origin + ": no sections - a test with no '--- OUT --->' asserts nothing";
        return false;
    }

    // the header is everything above the first delimiter, walked up to that very record - `number` is
    // a line number for a message, never an index to compute back from
    std::set<std::string> seen_settings;
    for (const LineRecord &record : lines) {
        if (&record == headers.front().line) {
            break;
        }

        if (!read_setting(origin, record, seen_settings, out_file, out_error)) {
            return false;
        }
    }

    bool has_output = false;
    std::set<std::string> seen_sections;

    for (size_t i = 0; i < headers.size(); i += 1) {
        const LineRecord &header = *headers[i].line;
        const std::string &name = headers[i].name;

        if (!seen_sections.insert(name).second) {
            out_error = locate(origin, header.number, "section '" + name + "' appears twice");
            return false;
        }

        // sliced out of the file rather than rebuilt from lines, so an OUT golden is byte exact
        const size_t body_begin = header.next;
        const size_t body_end = i + 1 < headers.size() ? headers[i + 1].line->begin : content.size();
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

        // matched against dump_section_name rather than against a second table of the same names: a
        // dump kind is added by extending that one switch, and a name spelled here as well is a name
        // this parser and the runner's section titles could disagree about. the message enumerates
        // the same table, so it cannot list a name the parser does not accept
        std::optional<DumpKind> kind;
        for (const DumpKind candidate : k_dump_kinds) {
            if (name == dump_section_name(candidate)) {
                kind = candidate;
                break;
            }
        }

        if (!kind.has_value()) {
            std::string known = "OUT";
            for (const DumpKind candidate : k_dump_kinds) {
                known += std::string(", ") + dump_section_name(candidate);
            }

            out_error = locate(origin, header.number,
                "unknown section '" + name + "', expected one of: " + known);
            return false;
        }

        CheckSection section { kind.value(), {} };
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
