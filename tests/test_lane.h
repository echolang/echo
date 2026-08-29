#ifndef TESTS_TEST_LANE_H
#define TESTS_TEST_LANE_H

#pragma once

#include <string>
#include <string_view>

namespace EchoTests
{
    // two ways to run the suite, and the default is the honest one.
    //
    // `t_ci` asserts everything the corpus wrote: dump sections, `mode: build`, the lot. `./tests` with
    // no extra flags is this lane, so CI cannot silently go fast. `t_dev` is the local loop: it skips
    // IR/AST dumps and native links, which is a runner policy rather than a deleted `.test` section.
    //
    // `--dev` on the tests binary, or `ECO_TEST_LANE=dev` in the environment. The env exists so a
    // wrapper that cannot splice argv (ctest, a script) still has a knob
    enum class TestLane
    {
        t_ci,
        t_dev
    };

    void set_test_lane(TestLane lane);

    TestLane current_test_lane();

    inline bool is_dev_lane() {
        return current_test_lane() == TestLane::t_dev;
    }

    // how many echoc children the e2e pool starts. `ECO_E2E_JOBS` wins; otherwise hardware
    // concurrency, capped under ASan (Debug is ASan, and a dozen ASan compilers thrash) and on
    // Windows (a native build then starts clang)
    unsigned e2e_worker_count();

    // where the suite finds the compiler, the corpus, and scratch. compile-time defaults from
    // CMake; `ECHOC_BINARY` / `ECO_E2E_TESTS_DIR` / `ECO_E2E_TMP_DIR` in the environment win, so
    // a tests binary moved off the build machine (CI artifacts) can still find them
    const char *echoc_binary();
    const char *e2e_tests_dir();
    const char *e2e_tmp_dir();

    // substring of the corpus-relative path (`structs/init_class.eco`). empty means every case.
    // `--e2e-filter` on the tests binary, or `ECO_E2E_FILTER`
    void set_e2e_filter(std::string filter);
    const std::string &e2e_filter();

    // `--shard i/n` or `ECO_E2E_SHARD=i/n`. `n == 1` means no split
    void set_e2e_shard(unsigned index, unsigned count);
    unsigned e2e_shard_index();
    unsigned e2e_shard_count();

    // `i/n` with 0 <= i < n. false with a sentence
    bool parse_e2e_shard(std::string_view text, unsigned &index, unsigned &count, std::string &error);

    // does this corpus case run on this invocation? filter then shard, in that order, so a
    // filtered run is not silently emptied by a shard that does not contain it
    bool e2e_case_selected(std::string_view relative_path, size_t index);
};

#endif
