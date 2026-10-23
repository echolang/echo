#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#include "test_lane.h"

int main(int argc, char *argv[])
{
    Catch::Session session;

    // **the hash seed, pinned for every program this suite runs.** a seeded `map` iterates in an order
    // that moves from run to run, and a golden that prints one would flake. not overwritten, so a case
    // can still say `env: ECHO_HASH_SEED=7`
#if defined(_WIN32)
    if (std::getenv("ECHO_HASH_SEED") == nullptr) {
        _putenv_s("ECHO_HASH_SEED", "0");
    }
#else
    setenv("ECHO_HASH_SEED", "0", 0);
#endif

    bool dev = false;
    std::string filter;
    std::string shard;

    using namespace Catch::Clara;

    auto cli = session.cli()
        | Opt(dev)["--dev"](
            "fast local lane: skip IR/AST dumps and mode:build. "
            "CI must not pass this. ECO_TEST_LANE=dev is the same knob")
        | Opt(filter, "substr")["--e2e-filter"](
            "run only e2e cases whose corpus-relative path contains this substring")
        | Opt(shard, "i/n")["--shard"](
            "run e2e slice i of n (0-based). ECO_E2E_SHARD=i/n is the same knob");

    session.cli(cli);

    const int rc = session.applyCommandLine(argc, argv);
    if (rc != 0) {
        return rc;
    }

    if (dev) {
        EchoTests::set_test_lane(EchoTests::TestLane::t_dev);
    }

    if (!filter.empty()) {
        EchoTests::set_e2e_filter(std::move(filter));
    }

    if (shard.empty()) {
        if (const char *env = std::getenv("ECO_E2E_SHARD"); env != nullptr && *env != '\0') {
            shard = env;
        }
    }

    if (!shard.empty()) {
        unsigned index = 0;
        unsigned count = 1;
        std::string error;
        if (!EchoTests::parse_e2e_shard(shard, index, count, error)) {
            std::cerr << error << "\n";
            return 1;
        }

        EchoTests::set_e2e_shard(index, count);
    }

    return session.run();
}
