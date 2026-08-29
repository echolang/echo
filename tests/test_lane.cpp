#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "test_lane.h"

namespace
{
    EchoTests::TestLane g_lane = EchoTests::TestLane::t_ci;
    bool g_lane_set = false;
    std::string g_filter;
    bool g_filter_set = false;
    unsigned g_shard_index = 0;
    unsigned g_shard_count = 1;
    bool g_shard_set = false;

    const char *env_or(const char *name, const char *fallback)
    {
        if (const char *value = std::getenv(name); value != nullptr && *value != '\0') {
            return value;
        }

        return fallback;
    }
};

void EchoTests::set_test_lane(TestLane lane)
{
    g_lane = lane;
    g_lane_set = true;
}

EchoTests::TestLane EchoTests::current_test_lane()
{
    if (g_lane_set) {
        return g_lane;
    }

    if (const char *env = std::getenv("ECO_TEST_LANE"); env != nullptr) {
        if (std::string_view(env) == "dev") {
            return TestLane::t_dev;
        }
    }

    return TestLane::t_ci;
}

#ifndef ECHO_TESTS_ECHOC_BINARY
#define ECHO_TESTS_ECHOC_BINARY "echoc"
#endif

#ifndef ECHO_TESTS_E2E_DIR
#define ECHO_TESTS_E2E_DIR "tests_eco"
#endif

#ifndef ECHO_TESTS_TMP_DIR
#define ECHO_TESTS_TMP_DIR "e2e_tmp"
#endif

const char *EchoTests::echoc_binary()
{
    return env_or("ECHOC_BINARY", ECHO_TESTS_ECHOC_BINARY);
}

const char *EchoTests::e2e_tests_dir()
{
    return env_or("ECO_E2E_TESTS_DIR", ECHO_TESTS_E2E_DIR);
}

const char *EchoTests::e2e_tmp_dir()
{
    return env_or("ECO_E2E_TMP_DIR", ECHO_TESTS_TMP_DIR);
}

unsigned EchoTests::e2e_worker_count()
{
    if (const char *jobs = std::getenv("ECO_E2E_JOBS"); jobs != nullptr && *jobs != '\0') {
        char *end = nullptr;
        const unsigned parsed = static_cast<unsigned>(std::strtoul(jobs, &end, 10));

        if (end != jobs && parsed > 0) {
            return parsed;
        }
    }

    const unsigned detected = std::thread::hardware_concurrency();
    unsigned workers = detected == 0 ? 4 : detected;

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    // Debug is ASan in this tree. twelve ASan echocs compiling the stdlib at once is a memory
    // storm, not a speedup: the 1.5 s Debug compile turns into tens of seconds of paging
    if (workers > 4) {
        workers = 4;
    }
#endif

#if defined(_WIN32)
    if (workers > 8) {
        workers = 8;
    }
#endif

    return workers;
}

void EchoTests::set_e2e_filter(std::string filter)
{
    g_filter = std::move(filter);
    g_filter_set = true;
}

const std::string &EchoTests::e2e_filter()
{
    if (g_filter_set) {
        return g_filter;
    }

    if (const char *env = std::getenv("ECO_E2E_FILTER"); env != nullptr) {
        g_filter = env;
        g_filter_set = true;
    }

    return g_filter;
}

bool EchoTests::parse_e2e_shard(
    std::string_view text,
    unsigned &index,
    unsigned &count,
    std::string &error)
{
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        error = " --shard expects i/n, got '" + std::string(text) + "'";
        return false;
    }

    const std::string owned(text);
    char *end_index = nullptr;
    char *end_count = nullptr;
    index = static_cast<unsigned>(std::strtoul(owned.c_str(), &end_index, 10));
    count = static_cast<unsigned>(std::strtoul(owned.c_str() + slash + 1, &end_count, 10));

    if (end_index != owned.c_str() + slash
        || end_count == owned.c_str() + slash + 1
        || *end_count != '\0'
        || count == 0
        || index >= count) {
        error = " --shard i/n needs 0 <= i < n, got '" + owned + "'";
        return false;
    }

    return true;
}

void EchoTests::set_e2e_shard(unsigned index, unsigned count)
{
    g_shard_index = index;
    g_shard_count = count == 0 ? 1 : count;
    g_shard_set = true;
}

unsigned EchoTests::e2e_shard_index()
{
    return g_shard_index;
}

unsigned EchoTests::e2e_shard_count()
{
    return g_shard_count;
}

bool EchoTests::e2e_case_selected(std::string_view relative_path, size_t index)
{
    const std::string &filter = e2e_filter();
    if (!filter.empty() && relative_path.find(filter) == std::string_view::npos) {
        return false;
    }

    const unsigned count = e2e_shard_count();
    if (count > 1 && (index % count) != e2e_shard_index()) {
        return false;
    }

    return true;
}
