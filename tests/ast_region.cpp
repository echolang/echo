#include <catch2/catch_test_macros.hpp>

#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTRegion.h"
#include "AST/FunctionDeclNode.h"

#include "helpers.h"

using namespace AST;

TEST_CASE("a concrete function is owned after the pipeline", "[region]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(): int32 { return 1; }\n"
        "echo f();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = EchoTests::decls_named(m, "f");
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0]->region_state == RegionState::t_owned);
    REQUIRE(m.files().first()->region_state == RegionState::t_owned);
}

TEST_CASE("a generic template stays open; its instance is owned", "[region]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $x): T { return $x; }\n"
        "echo id(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = EchoTests::decls_named(m, "id");
    REQUIRE(decls.size() == 2);

    bool saw_template = false;
    bool saw_instance = false;

    for (auto *decl : decls) {
        if (decl->is_generic()) {
            REQUIRE(decl->region_state == RegionState::t_open);
            saw_template = true;
        } else {
            REQUIRE(decl->region_state == RegionState::t_owned);
            saw_instance = true;
        }
    }

    REQUIRE(saw_template);
    REQUIRE(saw_instance);
}

TEST_CASE("an owned region refuses mutation", "[region]")
{
    REQUIRE(region_accepts_mutation(RegionState::t_open));
    // t_ready is in-progress: the walk itself still mints drops
    REQUIRE(region_accepts_mutation(RegionState::t_ready));
    REQUIRE_FALSE(region_accepts_mutation(RegionState::t_owned));
}
