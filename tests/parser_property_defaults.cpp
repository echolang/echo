#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTMemberLookup.h>
#include <AST/AssignNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::type_named;

namespace
{
    TypeDeclNode *require_type(Bundle &bundle, const std::string &name)
    {
        auto *decl = type_named(bundle.modules.find_module("test"), name);
        REQUIRE(decl != nullptr);
        return decl;
    }

    size_t init_assignments(const FunctionDeclNode *ctor)
    {
        REQUIRE(ctor != nullptr);
        REQUIRE(ctor->body != nullptr);

        size_t n = 0;
        for (const auto &child : ctor->body->children) {
            if (child.node() != nullptr && child.node()->get_node_type() == NodeType::n_assign) {
                n++;
            }
        }

        return n;
    }

    const AssignNode *nth_assign(const FunctionDeclNode *ctor, size_t index)
    {
        REQUIRE(ctor != nullptr);
        REQUIRE(ctor->body != nullptr);

        size_t seen = 0;
        for (const auto &child : ctor->body->children) {
            if (child.node() == nullptr || child.node()->get_node_type() != NodeType::n_assign) {
                continue;
            }

            if (seen == index) {
                return static_cast<const AssignNode *>(child.node());
            }

            seen++;
        }

        return nullptr;
    }
}

TEST_CASE("all property defaults synthesize a zero-arg constructor", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Opts {\n"
        "    int32 $a = 1;\n"
        "    int32 $b = 2;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *opts = require_type(*bundle, "Opts");
    auto *synth = opts->synthesized_constructor();
    REQUIRE(synth != nullptr);
    REQUIRE(synth->is_implicitly_generated);
    REQUIRE(synth->args.empty());
    REQUIRE(init_assignments(synth) == 2);

    // `$this` is the first child: a class constructor's allocation lives in that declaration,
    // and seating a field before it runs writes through a null handle
    REQUIRE(synth->body->children.front().node() != nullptr);
    REQUIRE(synth->body->children.front().node()->get_node_type() == NodeType::n_vardecl);
}

TEST_CASE("partial property defaults synthesize no constructor", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Mixed {\n"
        "    int32 $a = 1;\n"
        "    int32 $b;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *mixed = require_type(*bundle, "Mixed");
    REQUIRE(mixed->synthesized_constructor() == nullptr);
    REQUIRE(mixed->constructors().empty());
}

TEST_CASE("no property defaults keep the field-wise constructor", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    int32 $y;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *point = require_type(*bundle, "Point");
    REQUIRE(point->synthesized_constructor() != nullptr);
    REQUIRE(point->synthesized_constructor()->args.size() == 2);
}

TEST_CASE("an empty struct still gets the zero-arg field-wise constructor", "[property_defaults]")
{
    // vacuous "all have defaults" must not steal this: there are no defaults, there are no
    // properties, and Foo() has always been the field-wise constructor of an empty type
    auto bundle = EchoTests::tests_make_parsed_bundle("struct Empty { }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *empty = require_type(*bundle, "Empty");
    REQUIRE(empty->synthesized_constructor() != nullptr);
    REQUIRE(empty->synthesized_constructor()->args.empty());
}

TEST_CASE("private does not suppress the all-defaults zero-arg constructor", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Shut {\n"
        "    private int32 $n = 0;\n"
        "    int32 $m = 1;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *shut = require_type(*bundle, "Shut");
    auto *synth = shut->synthesized_constructor();
    REQUIRE(synth != nullptr);
    REQUIRE(synth->args.empty());
}

TEST_CASE("private still suppresses field-wise when nothing has a default", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Shut {\n"
        "    private int32 $n;\n"
        "    int32 $m;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *shut = require_type(*bundle, "Shut");
    REQUIRE(shut->synthesized_constructor() == nullptr);
}

TEST_CASE("a user constructor() occupies the all-defaults zero-arg signature", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Opts {\n"
        "    int32 $a = 1;\n"
        "    constructor() { $this->a = 9; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *opts = require_type(*bundle, "Opts");
    REQUIRE(opts->synthesized_constructor() == nullptr);
    REQUIRE(opts->constructors().size() == 1);

    auto *user = opts->constructors()[0];
    REQUIRE(user->args.empty());
    REQUIRE_FALSE(user->is_implicitly_generated);

    // default first, then the body write
    REQUIRE(init_assignments(user) == 2);

    auto *first = nth_assign(user, 0);
    REQUIRE(first != nullptr);
    REQUIRE(first->is_initialization);
    REQUIRE(first->value_expr != nullptr);
    REQUIRE(first->value_expr->get_node_type() == NodeType::n_literal_int);
}

TEST_CASE("defaults are prepended to a user constructor, not a copy constructor", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Gate {\n"
        "    int32 $id = 1;\n"
        "    int32 $chevrons;\n"
        "    constructor(int32 $chevrons) { $this->chevrons = $chevrons; }\n"
        "    constructor(Gate& $other) { $this->id = $other->id; $this->chevrons = $other->chevrons; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *gate = require_type(*bundle, "Gate");
    REQUIRE(gate->synthesized_constructor() == nullptr);
    REQUIRE(gate->constructors().size() == 2);

    auto *build = gate->constructors()[0];
    REQUIRE(build->args.size() == 1);
    REQUIRE(init_assignments(build) == 2);

    auto *first = nth_assign(build, 0);
    REQUIRE(first != nullptr);
    REQUIRE(first->is_initialization);
    REQUIRE(first->value_expr != nullptr);
    REQUIRE(first->value_expr->get_node_type() == NodeType::n_literal_int);

    auto *copy = gate->constructors()[1];
    REQUIRE(is_copy_constructor(copy, gate->value_type()));
    REQUIRE(init_assignments(copy) == 2);

    auto *copy_first = nth_assign(copy, 0);
    REQUIRE(copy_first != nullptr);
    REQUIRE(copy_first->value_expr != nullptr);
    // `$other->id`, not the default literal. a prepended default would be n_literal_int
    REQUIRE(copy_first->value_expr->get_node_type() == NodeType::n_member_access);
}

TEST_CASE("a generic type with all defaults synthesizes a zero-arg constructor", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> {\n"
        "    int32 $n = 1;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *box = require_type(*bundle, "Box");
    REQUIRE(box->synthesized_constructor() != nullptr);
    REQUIRE(box->synthesized_constructor()->args.empty());
    REQUIRE(box->synthesized_constructor()->is_generic());
}

TEST_CASE("cloned defaults consume the recipe on the property", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Opts {\n"
        "    int32 $a = 1;\n"
        "    int32 $b = 2;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *opts = require_type(*bundle, "Opts");
    REQUIRE(opts->properties().size() == 2);
    REQUIRE(opts->properties()[0]->init_expr == nullptr);
    REQUIRE(opts->properties()[1]->init_expr == nullptr);
}

TEST_CASE("unused mixed defaults keep the recipe", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Mixed {\n"
        "    int32 $a = 1;\n"
        "    int32 $b;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *mixed = require_type(*bundle, "Mixed");
    REQUIRE(mixed->properties()[0]->init_expr != nullptr);
    REQUIRE(mixed->properties()[1]->init_expr == nullptr);
}

TEST_CASE("a user constructor consumes the recipe", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Gate {\n"
        "    int32 $id = 1;\n"
        "    int32 $chevrons;\n"
        "    constructor(int32 $chevrons) { $this->chevrons = $chevrons; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *gate = require_type(*bundle, "Gate");
    REQUIRE(gate->properties()[0]->init_expr == nullptr);
}

TEST_CASE("a copy-only mixed type keeps the recipe", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Keep {\n"
        "    int32 $id = 1;\n"
        "    int32 $n;\n"
        "    constructor(Keep& $other) { $this->id = $other->id; $this->n = $other->n; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *keep = require_type(*bundle, "Keep");
    REQUIRE(keep->synthesized_constructor() == nullptr);
    REQUIRE(keep->properties()[0]->init_expr != nullptr);
}

TEST_CASE("a bodyless constructor does not consume an unused mixed default", "[property_defaults]")
{
    // prepend never cloned: there is no body to seat into. consume used to guess from
    // "a non-copy constructor exists" and drop the recipe, so TypeChecker never saw 'nope'
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Mixed {\n"
        "    int32 $a = 'nope';\n"
        "    int32 $b;\n"
        "    constructor(int32 $b);\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());

    auto *mixed = require_type(*bundle, "Mixed");
    REQUIRE(mixed->synthesized_constructor() == nullptr);
    REQUIRE(mixed->properties()[0]->init_expr != nullptr);
}

TEST_CASE("a closure in a property default is published onto the file root", "[property_defaults]")
{
    // the original was parsed in the declaration pass onto a scratch scope that is discarded.
    // prepend clones it in the body pass and publish_cloned_closures has to land the clone
    // where codegen emits bodies from
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box {\n"
        "    function<int32()> $f = function() : int32 { return 7; };\n"
        "}\n"
        "Box $b = Box();\n"
        "echo $b->f();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    FunctionDeclNode *published = nullptr;

    for (auto *decl : m.nodes.of_type<FunctionDeclNode>()) {
        if (decl->is_closure && EchoTests::is_file_root_child(m, decl)) {
            published = decl;
            break;
        }
    }

    REQUIRE(published != nullptr);
    REQUIRE(published->body != nullptr);
}

TEST_CASE("a bad default reports once after it has been cloned", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bad {\n"
        "    int32 $n = 'nope';\n"
        "}\n"
        "Bad $b = Bad();\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(bundle->collector.issues.size() == 1);
}

TEST_CASE("an unused mixed default is still type-checked", "[property_defaults]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Mixed {\n"
        "    int32 $n = 'nope';\n"
        "    int32 $m;\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(bundle->collector.issues.size() == 1);
}
