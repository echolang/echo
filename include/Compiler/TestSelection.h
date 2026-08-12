#ifndef TESTSELECTION_H
#define TESTSELECTION_H

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // what a `--filter` word selects on. **a closed vocabulary**, read out of one table by
    // Compiler::parse_test_filter, so a misspelled tag is refused at the word that is wrong rather than
    // taken for a test name nothing carries
    enum class TestFilterKind
    {
        t_name,
        t_group,
        t_file,
        t_module
    };

    // one `--filter` word, settled. `t_name` is what a bare word means, which is the common case spelled
    // shortest - the same call `#[depends:]`'s bare path makes
    struct TestFilter
    {
        TestFilterKind kind = TestFilterKind::t_name;
        std::string value;
    };

    // `group:parsing`, or a bare `adds_up`. false with a sentence naming the tags there were.
    //
    // it goes through Compiler::split_scheme for the tagged half, so this refusal and `--link`'s are one
    // wording; a word with no colon in it never reaches that function, a bare name being legal here where
    // `--link` has no such shape
    bool parse_test_filter(const std::string &spelled, TestFilter &out, std::string &out_error);

    // one test, as the driver knows it: everything a filter can select on plus the symbol to call.
    //
    // **flat, and deliberately not a pointer into the AST.** What runs a test is a fork and an address, and
    // what chooses one is four strings - neither wants a FunctionDeclNode, and keeping one here would make
    // the runner's lifetime depend on the bundle's
    struct TestCase
    {
        std::string module;
        std::filesystem::path file;
        std::string group;
        std::string name;

        // the mangled name to look the compiled body up under
        std::string symbol;
    };

    // `<module>/<file>::<name>`, which is a test's identity said in full - the three things it is tagged by
    // that a runner reports and a `--filter` narrows. One owner, because it is written by the reporter and
    // read by a person comparing it against what they typed
    std::string test_display_name(const TestCase &test);

    // `<module>/<file>`, the half of a display name a grouped listing puts on its header so the tests under
    // it need only their own names. **Beside test_display_name and not spelled at the reporter**, or a
    // header and a failure block would name one file two ways
    std::string test_display_file(const TestCase &test);

    // what an invocation asked to run: the filters from the command line and from any declared test target,
    // already parsed.
    //
    // **empty means everything.** A test selected by *any* filter runs, so two `group:` words run both
    // groups rather than intersecting to nothing - which is what "the filters add up" means at the option
    struct TestSelection
    {
        std::vector<TestFilter> filters;

        bool is_everything() const {
            return filters.empty();
        }

        // the *declared* medium of the same vocabulary: a `#[target: test]`'s `groups:` and `files:`, as the
        // filters they stand for.
        //
        // **here rather than in the driver**, beside the tag table `--filter` is read through, because that
        // is what makes a declared target a saved filter instead of a second engine - a field added to the
        // vocabulary is added once, and cannot compile while quietly selecting nothing. Taken as the two
        // lists rather than as a Parser::ModuleTarget so a selection stays a thing the manifest reader is
        // not in the include graph of
        void add_declared(
            const std::vector<std::string> &groups,
            const std::vector<std::filesystem::path> &files);
    };

    // the selected subset, in the order given. **the one selection engine**, and both spellings of a
    // selection reach it: `--filter` on the command line and `groups`/`files` on a `#[target: test]`, which
    // is the same one-vocabulary-two-media split `#[link:]` and `--link` have
    std::vector<TestCase> select_tests(const std::vector<TestCase> &all, const TestSelection &selection);
};

#endif
