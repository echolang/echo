#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTIssue.h>
#include <AST/GuardNode.h>
#include <AST/ScopeNode.h>
#include <AST/VarDeclNode.h>
#include <string>

#include "helpers.h"

// `guard` as an **initializer form on an ordinary declaration** - `T $x = guard <nullable> else { ... }`.
//
// Parser::parse_varexpr owns the type, the name, the `=` and the registration; Parser::parse_guard owns
// everything from the keyword to the else arm's closing brace. what these cases pin is the seam between
// them, and one invariant whose failure is silent.

namespace
{
    const std::string HALVE =
        "function halve(int32 $n) : int32? { if ($n < 0) { return null; } return $n / 2; }\n";

    AST::GuardNode *first_guard(AST::Bundle &bundle)
    {
        for (auto *guard : bundle.modules.find_module("test").nodes.of_type<AST::GuardNode>()) {
            return guard;
        }

        return nullptr;
    }
}

TEST_CASE("A guard binding is registered by name without becoming a statement", "[parser][nullability]")
{
    // **the invariant whose failure is a leaked retain.** the binding's initializer runs once, inside
    // the branch that found a value - so the declaration must be registered by *name* only
    // (AST::ScopeNode::declare_variable) and must not also be appended to the scope's child list, or
    // codegen emits that initializer a second time as an ordinary statement.
    //
    // nothing reports it if this goes wrong: the program runs and the count is one too high
    auto bundle = EchoTests::tests_make_parsed_bundle(
        HALVE +
        "function unwrap(int32 $n) : int32\n"
        "{\n"
        "    int32 $v = guard halve($n) else { return -1; }\n"
        "    return $v;\n"
        "}\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::GuardNode *guard = first_guard(*bundle);
    REQUIRE(guard != nullptr);
    REQUIRE(guard->decl != nullptr);

    // the name resolves for every statement after the guard...
    auto &module = bundle->modules.find_module("test");
    AST::ScopeNode *body = nullptr;

    for (auto *scope : module.nodes.of_type<AST::ScopeNode>()) {
        if (scope->lookup_variable("$v").found_in_frame()) {
            body = scope;
            break;
        }
    }

    REQUIRE(body != nullptr);

    // ...and yet the declaration is **not** one of that scope's statements. the guard node is
    for (const auto &child : body->children) {
        REQUIRE(child.unsafe_ptr<AST::Node>() != static_cast<AST::Node *>(guard->decl));
    }
}

TEST_CASE("A guard binding takes the payload's type, written or inferred", "[parser][nullability]")
{
    // the declaration's type is the *unwrapped* one, which is the whole point of the form: from the
    // guard onwards the name certainly has a value. inferred through AST::infer_declaration_type over
    // the payload, so the `const` an author wrote survives onto it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        HALVE +
        "function unwrap(int32 $n) : int32\n"
        "{\n"
        "    $written = guard halve($n) else { return -1; }\n"
        "    const $konst = guard halve($n) else { return -2; }\n"
        "    return $written + $konst;\n"
        "}\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    size_t seen = 0;

    for (auto *decl : module.nodes.of_type<AST::VarDeclNode>()) {
        if (decl->name_full() == "$written") {
            REQUIRE(decl->type().is_primitive_of_type(AST::ValueTypePrimitive::t_int32));
            REQUIRE_FALSE(decl->type().is_nullable());
            REQUIRE_FALSE(decl->type().is_const());

            // the one bit that tells AST::TypeChecker this declaration's initializer is legitimately
            // one level more nullable than the declaration is
            REQUIRE(decl->binds_unwrapped);
            seen++;
        }

        if (decl->name_full() == "$konst") {
            REQUIRE(decl->type().is_primitive_of_type(AST::ValueTypePrimitive::t_int32));
            REQUIRE_FALSE(decl->type().is_nullable());
            REQUIRE(decl->type().is_const());
            REQUIRE(decl->binds_unwrapped);
            seen++;
        }
    }

    REQUIRE(seen == 2);
}

TEST_CASE("The rendered guard node is unchanged by the move", "[parser][nullability]")
{
    // the dump is a rendering of the *node*, not of the source, and the node is the same node. that is
    // what keeps four RAST goldens intact across a syntax change
    auto d = EchoTests::tests_make_node_description(
        HALVE +
        "function unwrap(int32 $n) : int32\n"
        "{\n"
        "    int32 $v = guard halve($n) else { return -1; }\n"
        "    return $v;\n"
        "}\n");

    REQUIRE(d.find("guard vardecl<type<int32>>($v) = call halve(") != std::string::npos);
}

TEST_CASE("guard declares and never assigns", "[parser][nullability]")
{
    // writing into a name that already holds storage would owe that value an end on the path that binds
    // and leave it alone on the path that leaves, which are two different programs
    auto bundle = EchoTests::tests_make_parsed_bundle(
        HALVE +
        "int32 $existing = 5;\n"
        "$existing = guard halve(10) else { die('no'); }\n");

    REQUIRE(EchoTests::has_issue_containing(*bundle, "'guard' can only introduce a new declaration"));
}

TEST_CASE("guard is not an expression", "[parser][nullability]")
{
    // and must not become one: the else arm may hold a bare `return` precisely because it can never
    // produce a value, so an expression form would make that `return` read two ways at once
    auto bundle = EchoTests::tests_make_parsed_bundle(
        HALVE +
        "function unwrap(int32 $n) : int32 { return halve(guard halve($n) else { return -1; }); }\n");

    REQUIRE(EchoTests::has_issue_containing(*bundle, "'guard' is not an expression"));
}

TEST_CASE("the old statement spelling says where the name goes", "[parser][nullability]")
{
    // one spelling only, and a `guard` at the head of a statement can only be the old one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        HALVE +
        "guard int32 $v = halve(10) else { die('no'); }\n");

    REQUIRE(EchoTests::has_issue_containing(
        *bundle, "'guard' introduces a declaration's initializer"));
}
