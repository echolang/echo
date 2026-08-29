#include <catch2/catch_test_macros.hpp>

#include <AST/ASTMemberLookup.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

#include <string>
#include <vector>

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

TEST_CASE("init is registered on its type, in neither lookup structure", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    init { $this->hash = $this->age * 2; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *person = type_named(bundle->modules.find_module("test"), "Person");
    REQUIRE(person != nullptr);

    auto *init = find_init(&person->complex_type());
    REQUIRE(init != nullptr);
    REQUIRE(init->is_init());
    REQUIRE(init->owner_type == &person->complex_type());
    REQUIRE(init->body != nullptr);
    REQUIRE(init->func_name() == "$init");

    REQUIRE(find_member_functions(&person->complex_type(), "init").empty());
    REQUIRE(find_member_functions(&person->complex_type(), "$init").empty());
    REQUIRE(bundle->collector.functions.overloads("init", *person->ast_namespace).empty());
}

TEST_CASE("init's receiver is a borrow, and it is its only parameter", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    init { $this->hash = $this->age; }\n"
        "}\n");

    auto *init = find_init(&type_named(bundle->modules.find_module("test"), "Person")->complex_type());
    REQUIRE(init != nullptr);
    REQUIRE(init->args.size() == 1);
    REQUIRE(init->implicit_arg_count() == 1);
    REQUIRE(init->args[0]->name_full() == "$this");
    REQUIRE(init->args[0]->type().is_pointer());
    REQUIRE_FALSE(init->args[0]->type().is_nullable());
}

TEST_CASE("a second init is refused", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $n;\n"
        "    init { $this->n = 1; }\n"
        "    init { $this->n = 2; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "already has an init"));
}

TEST_CASE("init on an enum or interface is refused", "[init]")
{
    auto en = EchoTests::tests_make_parsed_bundle(
        "enum E {\n"
        "    case a;\n"
        "    init { }\n"
        "}\n");
    REQUIRE(has_issue_containing(*en, "cannot declare an init"));

    auto iface = EchoTests::tests_make_parsed_bundle(
        "interface I {\n"
        "    init { }\n"
        "}\n");
    REQUIRE(has_issue_containing(*iface, "cannot declare an init"));
}

TEST_CASE("derived-field constructor arity does not depend on file order", "[init]")
{
    // pass 3 used to finalize each type as its file was parsed, so a call in an earlier file
    // bound against the wide memberwise list. both orders have to produce the same arity
    const std::string type_file =
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    init { $this->hash = $this->age * 2; }\n"
        "}\n";
    const std::string call_file = "Person $p = Person(7);\n";

    auto type_first = EchoTests::tests_make_parsed_bundle(
        std::vector<std::string>{ type_file, call_file });
    REQUIRE_FALSE(type_first->collector.has_critical_issues());
    REQUIRE(type_named(type_first->modules.find_module("test"), "Person")
        ->synthesized_constructor()->args.size() == 1);

    auto call_first = EchoTests::tests_make_parsed_bundle(
        std::vector<std::string>{ call_file, type_file });
    REQUIRE_FALSE(call_first->collector.has_critical_issues());
    REQUIRE(type_named(call_first->modules.find_module("test"), "Person")
        ->synthesized_constructor()->args.size() == 1);
}

TEST_CASE("a constructor that seats through a method on $this is fully assigned", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $x, int32 $y) { $this->seat($x, $y); }\n"
        "    function seat(int32 $x, int32 $y) : void {\n"
        "        $this->x = $x;\n"
        "        $this->y = $y;\n"
        "    }\n"
        "}\n"
        "Point $p = Point(1, 2);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("init that seats through a method still sees the helper's reads", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    constructor() {}\n"
        "    init { $this->seed(); }\n"
        "    function seed() : void { $this->hash = $this->age; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "reads 'age' before every constructor has assigned it"));
}

TEST_CASE("a helper that assigns a derived field on only one branch is some-paths", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    init { $this->maybe_hash(); }\n"
        "    function maybe_hash() : void {\n"
        "        if ($this->age > 0) { $this->hash = 1; }\n"
        "    }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "not assigned on all paths of 'init'"));
}

TEST_CASE("a helper that leaves a field blank is still a constructor hole", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $x) { $this->seat($x); }\n"
        "    function seat(int32 $x) : void { $this->x = $x; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "not assigned on all paths of this constructor"));
}

TEST_CASE("a free function that takes T& does not assign through $this", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $x, int32 $y) { seat($this, $x, $y); }\n"
        "}\n"
        "function seat(Point& $p, int32 $x, int32 $y) : void {\n"
        "    $p->x = $x;\n"
        "    $p->y = $y;\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "not assigned on all paths of this constructor"));
}

TEST_CASE("a user constructor without init must still assign every field", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "    constructor(int32 $x) { $this->x = $x; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "not assigned on all paths of this constructor"));
}

TEST_CASE("init omits a derived field from the implicit constructor", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    init { $this->hash = $this->age * 2; }\n"
        "}\n"
        "Person $p = Person(7);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *person = type_named(bundle->modules.find_module("test"), "Person");
    REQUIRE(person->synthesized_constructor() != nullptr);
    REQUIRE(person->synthesized_constructor()->args.size() == 1);
    REQUIRE(person->synthesized_constructor()->args[0]->name() == "age");
}

TEST_CASE("a private field derived in init does not refuse the implicit constructor", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    private int32 $hash;\n"
        "    init { $this->hash = $this->age * 2; }\n"
        "}\n"
        "Person $p = Person(7);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
    REQUIRE(type_named(bundle->modules.find_module("test"), "Person")->synthesized_constructor() != nullptr);
}

TEST_CASE("init assigning a field on only some paths is not derived", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    init {\n"
        "        if ($this->age > 0) { $this->hash = 1; }\n"
        "    }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "not assigned on all paths of 'init'"));
}

TEST_CASE("a second copy constructor is refused even when labels differ", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box {\n"
        "    int32 $n;\n"
        "    constructor(Box& $other) { $this->n = $other->n; }\n"
        "    constructor(from: Box& $another) { $this->n = $another->n; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "already has a copy constructor"));
}

TEST_CASE("init reading a field through a cast is still a read", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $age;\n"
        "    int32 $hash;\n"
        "    constructor() {}\n"
        "    init { $this->hash = $this->age as int32; }\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "reads 'age' before every constructor has assigned it"));
}

TEST_CASE("return in init is a void return, not $this", "[init]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $n;\n"
        "    int32 $flag;\n"
        "    constructor(int32 $n) { $this->n = $n; }\n"
        "    init {\n"
        "        if ($this->n < 0) { $this->flag = 0; return; }\n"
        "        $this->flag = 1;\n"
        "    }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("init refuses a parameter list and a return type", "[init]")
{
    auto params = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $n;\n"
        "    init() { $this->n = 1; }\n"
        "}\n");
    REQUIRE(has_issue_containing(*params, "init takes no parameter list"));

    auto ret = EchoTests::tests_make_parsed_bundle(
        "struct Person {\n"
        "    int32 $n;\n"
        "    init : int32 { $this->n = 1; }\n"
        "}\n");
    REQUIRE(has_issue_containing(*ret, "init returns nothing"));
}
