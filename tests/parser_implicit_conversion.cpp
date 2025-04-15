#include <catch2/catch_test_macros.hpp>

#include <AST/ASTArgumentFit.h>
#include <AST/ASTBundle.h>
#include <AST/ASTMemberLookup.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

namespace
{
    // a type offering a borrowed window over itself, with the conversion method deliberately *not*
    // named `view`: the compiler used to recognise it by that spelling, and nothing about these
    // assertions would change if it still did unless the name is something else entirely
    const char *k_buffer =
        "struct Window { usize $count; }\n"
        "struct Buffer {\n"
        "    usize $count;\n"
        "    #[implicit]\n"
        "    function window() : Window { return Window($this->count); }\n"
        "    function window(usize $from, usize $count) : Window { return Window($count); }\n"
        "}\n";
}

// this is the one test that pins *where in the parse passes* the conversion is published. the
// attribute is staged on a scope and drained by the declaration it was written for, which has to
// happen before the declaration registers - a future refactor moving that drain back below the
// function body would leave every end-to-end case passing and this one failing
TEST_CASE("an #[implicit] method is published on its type", "[implicit_conversion]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_buffer);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *buffer = type_named(m, "Buffer");
    auto *window = type_named(m, "Window");
    REQUIRE(buffer != nullptr);
    REQUIRE(window != nullptr);

    // exactly one, published once despite both parse passes reaching the declaration
    const auto &published = buffer->complex_type().implicit_conversions();
    REQUIRE(published.size() == 1);

    // ...and it is the parameterless one. the two-parameter `window` is an ordinary overload of the
    // same name, which is the whole point of the marker: the name says nothing
    REQUIRE(published[0]->args.size() == published[0]->implicit_arg_count());
    REQUIRE(published[0]->get_return_type() == window->value_type());

    // the lookup answers with it, and only for that target
    REQUIRE(find_implicit_conversion(buffer->value_type(), window->value_type()) == published[0]);
    REQUIRE(find_implicit_conversion(window->value_type(), buffer->value_type()) == nullptr);

    // never to itself: t_exact answers identity long before the conversion arm is reached, so
    // admitting it here would be a conversion that can never fire
    REQUIRE(find_implicit_conversion(buffer->value_type(), buffer->value_type()) == nullptr);
}

TEST_CASE("a marked method that cannot be a conversion is refused", "[implicit_conversion]")
{
    SECTION("with parameters")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Window { usize $count; }\n"
            "struct Buffer {\n"
            "    usize $count;\n"
            "    #[implicit]\n"
            "    function window(usize $from) : Window { return Window($from); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "takes no parameters"));

        auto &m = bundle->modules.find_module("test");
        REQUIRE(type_named(m, "Buffer")->complex_type().implicit_conversions().empty());
    }

    SECTION("on a free function")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Window { usize $count; }\n"
            "#[implicit]\n"
            "function window() : Window { return Window(1); }\n");

        REQUIRE(has_issue_containing(*bundle, "Only a method can declare"));
    }

    SECTION("twice to the same target")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Window { usize $count; }\n"
            "struct Buffer {\n"
            "    usize $count;\n"
            "    #[implicit]\n"
            "    function window() : Window { return Window($this->count); }\n"
            "    #[implicit]\n"
            "    function whole() : Window { return Window($this->count); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "already declares an implicit conversion"));

        // the first one still stands - a refusal declines to publish, it does not unpublish
        auto &m = bundle->modules.find_module("test");
        REQUIRE(type_named(m, "Buffer")->complex_type().implicit_conversions().size() == 1);
    }

    SECTION("on a generic owner")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Slice<T> { ptr<T> $at; }\n"
            "struct Array<T> {\n"
            "    ptr<T> $at;\n"
            "    #[implicit]\n"
            "    function slice() : Slice<T> { return Slice<T>($this->at); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "not supported yet"));
    }
}
