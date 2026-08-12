#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTMangler.h>
#include <AST/ASTTest.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeNode.h>

#include "helpers.h"

// what `test <name> { ... }` *is*, at the level of the tree.
//
// the bodies here hold ordinary statements rather than `assert` calls: these helpers parse one module with no
// standard library, so `assert` is a name nothing declares. What a real one does is the corpus's to say
//
// **a test is a function**, and every one of these cases is about a consequence of that rather than about a
// feature of its own: it is an AST::FunctionDeclNode of no arguments returning void, it sits where a nested
// `function` sits, and its body is a function body - so the monomorphizer's fixpoint, AST::OwnershipPass,
// AST::TypeChecker and the bodies loop in codegen all reach it by walking what they already walk.
//
// the corpus under tests_eco/tests/ asserts what a test *does*; this asserts what one is

using EchoTests::is_file_root_child;
using EchoTests::tests_make_parsed_bundle;
using EchoTests::tests_make_parsed_bundle_with_tests;

namespace
{
    // the test declarations of the one module these cases build
    const std::vector<AST::TestDeclaration> &tests_of(AST::Bundle &bundle)
    {
        return bundle.modules.find_module("test").tests;
    }
};

// **the default is that a test block never reaches pass 1 at all.** Every helper but the one named
// `_with_tests` builds a parser that drops them, which is what every other invocation of echoc does
TEST_CASE("a test block is dropped unless the module compiles its tests", "[testdecl]")
{
    // a **parse** error and not only an unresolvable name: `$b = ;` is not a statement, so a build that
    // reached this body at all would report it. That is what makes the assertion below an assertion
    auto bundle = tests_make_parsed_bundle(
        "test never_seen {\n"
        "    $a = no_such_function(no_such_argument);\n"
        "    $b = ;\n"
        "}\n"
        "echo 1;\n");

    REQUIRE(tests_of(*bundle).empty());
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // and the same source *with* tests compiled reports it, so the case above cannot pass by the body
    // happening to be fine
    auto compiled = tests_make_parsed_bundle_with_tests(
        "test never_seen {\n"
        "    $b = ;\n"
        "}\n");

    REQUIRE(compiled->collector.has_critical_issues());
}

TEST_CASE("a test is a void function of no arguments", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(
        "test adds_up {\n"
        "    $a = 1 + 1;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
    REQUIRE(tests_of(*bundle).size() == 1);

    const AST::TestDeclaration &declared = tests_of(*bundle).front();

    REQUIRE(declared.name == "adds_up");
    REQUIRE(declared.group.empty());
    REQUIRE(declared.decl != nullptr);
    REQUIRE(declared.decl->declared_in.file != nullptr);

    REQUIRE(declared.decl->is_test());
    REQUIRE(declared.decl->args.empty());
    REQUIRE(declared.decl->body != nullptr);
    REQUIRE(declared.decl->return_type != nullptr);
    REQUIRE(declared.decl->return_type->type.is_void());

    // **it is not a member of anything**, which is what keeps every method rule off it
    REQUIRE(declared.decl->owner_type == nullptr);
}

// the declaration goes to the file root and not into whatever scope it was written in - the rule a nested
// `function` already follows, and the reason both codegen and AST::OwnershipPass find it without an arm
TEST_CASE("a test's declaration is a child of the file root", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(
        "test adds_up {\n"
        "    $a = 1;\n"
        "}\n");

    REQUIRE(is_file_root_child(bundle->modules.find_module("test"), tests_of(*bundle).front().decl));
}

// **unspellable, so nothing needs a rule forbidding a call to one.** The `$` is the whole of it: no
// identifier can hold one, which is the same trick a closure's `closure$N` uses
TEST_CASE("a test's name is one no program can write", "[testdecl]")
{
    REQUIRE(AST::test_function_name("adds_up") == "test$adds_up");

    auto bundle = tests_make_parsed_bundle_with_tests(
        "test adds_up {\n"
        "    $a = 1;\n"
        "}\n");

    REQUIRE(tests_of(*bundle).front().decl->func_name() == "test$adds_up");
}

TEST_CASE("a group is read off the declaration's attributes", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(
        "#[group: \"arithmetic\"]\n"
        "test adds_up {\n"
        "    $a = 1;\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
    REQUIRE(tests_of(*bundle).front().group == "arithmetic");
}

// **the same name in two files is legal and is the design.** A test's name is unique within its own file and
// nowhere wider, which is what lets a file's tests be named without prefixing every one with where it lives
TEST_CASE("two files may each declare a test of one name", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(std::vector<std::string> {
        "test adds_up {\n    $a = 1 + 1;\n}\n",
        "test adds_up {\n    $a = 2 + 2;\n}\n"
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
    REQUIRE(tests_of(*bundle).size() == 2);

    const AST::TestDeclaration &first = tests_of(*bundle).at(0);
    const AST::TestDeclaration &second = tests_of(*bundle).at(1);

    REQUIRE(first.name == second.name);
    REQUIRE(first.decl->declared_in.file != second.decl->declared_in.file);

    // and they are two symbols, which is the half a shared name would have broken: the mangled name carries
    // the discriminated namespace the declaration was minted into, and TypeLowering refuses two declarations
    // mangling to one name rather than emitting them
    REQUIRE(AST::mangle_function_name(first.decl) != AST::mangle_function_name(second.decl));
}

TEST_CASE("a name declared twice in one file is refused", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(
        "test twice {\n    $a = 1;\n}\n"
        "test twice {\n    $a = 1;\n}\n");

    REQUIRE(EchoTests::has_issue_containing(*bundle, "already declares a test called 'twice'"));
}

// **the body is a function body**, so a file-scope local is as unreachable from inside one as it is from
// inside any other function - `is_function_boundary` is the whole of that and needs no rule of its own
TEST_CASE("a test body cannot reach a file-scope local", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(
        "$outer = 1;\n"
        "test reads_it {\n"
        "    $a = $outer;\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

// and nothing declared inside one leaks out: the body opens a lexical namespace keyed on its own brace, which
// no path can spell, exactly as a function body's does
TEST_CASE("a struct declared in a test body is not nameable outside it", "[testdecl]")
{
    auto bundle = tests_make_parsed_bundle_with_tests(
        "test declares_a_fixture {\n"
        "    struct Fixture { int32 $x; }\n"
        "    Fixture $f = Fixture(1);\n"
        "    echo $f->x;\n"
        "}\n"
        "function outside() : int32\n"
        "{\n"
        "    Fixture $f = Fixture(2);\n"
        "    return $f->x;\n"
        "}\n"
        "echo outside();\n");

    REQUIRE(bundle->collector.has_critical_issues());
}
