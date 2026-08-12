#include "Compiler/TestSelection.h"

#include "Compiler/LinkRequirement.h"

#include <fmt/core.h>

namespace
{
    // the tags a filter may carry, spelled once. `name` is in the table as well as being the bare default,
    // so `--filter name:group` can select a test actually called `group`
    const std::vector<std::pair<std::string, Compiler::TestFilterKind>> &filter_tag_table()
    {
        static const std::vector<std::pair<std::string, Compiler::TestFilterKind>> table = {
            { "name", Compiler::TestFilterKind::t_name },
            { "group", Compiler::TestFilterKind::t_group },
            { "file", Compiler::TestFilterKind::t_file },
            { "module", Compiler::TestFilterKind::t_module },
        };

        return table;
    }

    // does `path` end in `written`, at a path separator or at its whole length.
    //
    // **a suffix and not a substring**, so `file:math.eco` matches `src/math.eco` and not `src/mymath.eco`:
    // a filter has to be able to name as little of a path as is unambiguous, and the boundary is what keeps
    // "as little" from also meaning "the tail of a longer name"
    bool path_ends_with(const std::string &path, const std::string &written)
    {
        if (written.empty() || written.size() > path.size()) {
            return false;
        }

        if (path.compare(path.size() - written.size(), written.size(), written) != 0) {
            return false;
        }

        if (path.size() == written.size()) {
            return true;
        }

        const char before = path[path.size() - written.size() - 1];

        return before == '/' || before == '\\';
    }

    // does this one test answer this one filter.
    //
    // **inside select_tests' translation unit and not on the header**, because a selection is a *union* of
    // its filters: a caller able to ask one at a time would be able to build an intersection out of them,
    // which is not what any of the two media that spell a selection mean
    bool test_matches(const Compiler::TestCase &test, const Compiler::TestFilter &filter)
    {
        switch (filter.kind) {
        case Compiler::TestFilterKind::t_name:
            return test.name == filter.value;

        // an empty group never matches, so `group:` cannot be a way of asking for the ungrouped ones -
        // there is no word for that and inventing one silently would be worse than the refusal at the
        // empty value
        case Compiler::TestFilterKind::t_group:
            return !test.group.empty() && test.group == filter.value;

        case Compiler::TestFilterKind::t_file:
            return path_ends_with(test.file.string(), filter.value);

        case Compiler::TestFilterKind::t_module:
            return test.module == filter.value;
        }

        return false;
    }
};

bool Compiler::parse_test_filter(const std::string &spelled, TestFilter &out, std::string &out_error)
{
    if (spelled.empty()) {
        out_error = "--filter needs something to select on";
        return false;
    }

    // **a word with no colon is a test's name**, which is the shape almost every filter has. Checked here
    // rather than by asking split_scheme and treating its refusal as "then it was a name": that would turn
    // a misspelled tag into a name nothing carries, silently, which is the one thing this refuses for
    if (spelled.find(':') == std::string::npos) {
        out.kind = TestFilterKind::t_name;
        out.value = spelled;
        return true;
    }

    return split_scheme(
        spelled, filter_tag_table(), "test filter",
        scheme_list_of(filter_tag_table()), out.kind, out.value, out_error);
}

void Compiler::TestSelection::add_declared(
    const std::vector<std::string> &groups,
    const std::vector<std::filesystem::path> &files
)
{
    for (const std::string &group : groups) {
        filters.push_back(TestFilter { TestFilterKind::t_group, group });
    }

    for (const std::filesystem::path &file : files) {
        filters.push_back(TestFilter { TestFilterKind::t_file, file.string() });
    }
}

std::string Compiler::test_display_name(const TestCase &test)
{
    return fmt::format("{}::{}", test_display_file(test), test.name);
}

std::string Compiler::test_display_file(const TestCase &test)
{
    return fmt::format("{}/{}", test.module, test.file.filename().string());
}

std::vector<Compiler::TestCase> Compiler::select_tests(
    const std::vector<TestCase> &all,
    const TestSelection &selection
)
{
    if (selection.is_everything()) {
        return all;
    }

    std::vector<TestCase> selected;

    for (const TestCase &test : all) {
        for (const TestFilter &filter : selection.filters) {
            if (test_matches(test, filter)) {
                selected.push_back(test);
                break;
            }
        }
    }

    return selected;
}
