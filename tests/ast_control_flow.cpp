#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTControlFlow.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/WhileStatementNode.h>

#include "helpers.h"

using namespace AST;

namespace
{
    // the body of `f`, which every case here writes its statements into. a function body rather than the
    // file root so a `return` is legal in every one of them
    ScopeNode &body_of(Bundle &bundle, const char *name = "f")
    {
        for (auto &module_ptr : bundle.modules) {
            for (auto *decl : module_ptr->nodes.of_type<FunctionDeclNode>()) {
                if (decl->func_name() == name && decl->body != nullptr) {
                    return *decl->body;
                }
            }
        }

        FAIL("no function body named " << name);
        throw std::runtime_error("unreachable");
    }

    ExitKind kind_of(const std::string &statements)
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "function f() : int32 {\n" + statements + "\n}\n");

        return scope_exit_kind(body_of(*bundle));
    }
}

TEST_CASE("a scope that falls out of its bottom", "[control-flow]")
{
    REQUIRE(kind_of("int32 $x = 1;") == ExitKind::t_none);
}

TEST_CASE("return and die leave the function", "[control-flow]")
{
    REQUIRE(kind_of("return 1;") == ExitKind::t_function);

    // `die` is recognised through AST::BuiltinKind off the resolved declaration, so it needs the stdlib
    // that declares it - which tests_make_parsed_bundle does not link. covered e2e instead. `assert` is
    // deliberately *not* on the list either way: it returns when it holds
    REQUIRE(kind_of("assert(true);") == ExitKind::t_none);
}

TEST_CASE("break and continue leave the scope but not the function", "[control-flow]")
{
    // written inside a loop, because the parser refuses one outside and builds no node at all. the loop
    // itself never guarantees anything ran, so the *outer* scope still falls through - which is the
    // second half of what this asserts
    REQUIRE(kind_of("while (true) { break; }") == ExitKind::t_none);

    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f() : int32 {\n"
        "    while (true) { break; }\n"
        "    return 0;\n"
        "}\n");

    auto &loop_body = *body_of(*bundle).children[0].get_ptr<WhileStatementNode>()->loop_scope;

    REQUIRE(scope_exit_kind(loop_body) == ExitKind::t_scope);
    REQUIRE(scope_always_exits(loop_body));
    REQUIRE_FALSE(scope_always_leaves_function(loop_body));
}

TEST_CASE("an if contributes the weaker of its two arms", "[control-flow]")
{
    REQUIRE(kind_of("if (true) { return 1; } else { return 2; }") == ExitKind::t_function);

    // one arm only says nothing about the other, so the statement after the `if` is reachable
    REQUIRE(kind_of("if (true) { return 1; }") == ExitKind::t_none);

    // one arm returning and the other breaking: the scope is left, the function is not
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f() : int32 {\n"
        "    while (true) {\n"
        "        if (true) { return 1; } else { break; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n");

    auto &loop_body = *body_of(*bundle).children[0].get_ptr<WhileStatementNode>()->loop_scope;

    REQUIRE(scope_exit_kind(loop_body) == ExitKind::t_scope);
}

TEST_CASE("a while never guarantees an exit", "[control-flow]")
{
    // not even `while (true)`. an arm reading otherwise would silently move the constructor parser's
    // answer, which is the caller that has to be most certain
    REQUIRE(kind_of("while (true) { return 1; }") == ExitKind::t_none);
}

TEST_CASE("a bare nested block leaves for its own children's reason", "[control-flow]")
{
    REQUIRE(kind_of("{ return 1; }") == ExitKind::t_function);
    REQUIRE(kind_of("{ int32 $x = 1; }") == ExitKind::t_none);
}

TEST_CASE("the first leaving statement decides, not the last", "[control-flow]")
{
    // a `return` written behind a `break` is dead code, not an answer - the walk has to stop at the
    // break or it reports t_function for a scope control actually leaves by breaking
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f() : int32 {\n"
        "    while (true) {\n"
        "        break;\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");

    auto &loop_body = *body_of(*bundle).children[0].get_ptr<WhileStatementNode>()->loop_scope;

    REQUIRE(scope_exit_kind(loop_body) == ExitKind::t_scope);
}

TEST_CASE("break outside a loop is refused, and no node is built", "[control-flow]")
{
    EchoTests::assert_code_emits_issue(
        "break;",
        "'break' is only meaningful inside a loop - there is no loop here to leave. a closure or a "
        "nested function does not inherit the loop it was written in");

    EchoTests::assert_code_emits_issue(
        "continue;",
        "'continue' is only meaningful inside a loop - there is no loop here to go back to. a closure "
        "or a nested function does not inherit the loop it was written in");

    // a closure does not inherit the loop it was written in
    EchoTests::assert_code_emits_issue(
        "int32 $i = 0;\n"
        "while ($i < 3) { $f = function() : void { break; }; $i = $i + 1; }\n",
        "'break' is only meaningful inside a loop - there is no loop here to leave. a closure or a "
        "nested function does not inherit the loop it was written in");
}
