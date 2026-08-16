#include <catch2/catch_test_macros.hpp>

#include <AST/ASTArgumentFit.h>
#include <AST/ASTBundle.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ASTValueType.h>
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

    SECTION("inbound with no parameter")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Quantity {\n"
            "    int64 $n;\n"
            "    #[implicit]\n"
            "    static function from() : Quantity { return Quantity(0); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "takes one parameter"));
        auto &m = bundle->modules.find_module("test");
        REQUIRE(type_named(m, "Quantity")->complex_type().implicit_conversions().empty());
    }

    SECTION("inbound returning something other than the owner")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Window { usize $count; }\n"
            "struct Quantity {\n"
            "    int64 $n;\n"
            "    #[implicit]\n"
            "    static function from(int32 $n) : Window { return Window(1); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "must return 'Quantity'"));
        auto &m = bundle->modules.find_module("test");
        REQUIRE(type_named(m, "Quantity")->complex_type().implicit_conversions().empty());
    }

    SECTION("inbound twice from the same source")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Quantity {\n"
            "    int64 $n;\n"
            "    #[implicit]\n"
            "    static function from(int32 $n) : Quantity { return Quantity($n); }\n"
            "    #[implicit]\n"
            "    static function wrap(int32 $n) : Quantity { return Quantity($n); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "already declares an implicit conversion from"));

        auto &m = bundle->modules.find_module("test");
        REQUIRE(type_named(m, "Quantity")->complex_type().implicit_conversions().size() == 1);
    }

    SECTION("inbound with a borrow parameter")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Quantity {\n"
            "    int64 $n;\n"
            "    #[implicit]\n"
            "    static function from(int32& $n) : Quantity { return Quantity(0); }\n"
            "}\n");

        REQUIRE(has_issue_containing(*bundle, "takes the source by value"));
        auto &m = bundle->modules.find_module("test");
        REQUIRE(type_named(m, "Quantity")->complex_type().implicit_conversions().empty());
    }
}

TEST_CASE("an inbound #[implicit] static is published on the destination", "[implicit_conversion]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Quantity {\n"
        "    int64 $n;\n"
        "    #[implicit]\n"
        "    static function from(int32 $n) : Quantity { return Quantity($n); }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *quantity = type_named(m, "Quantity");
    REQUIRE(quantity != nullptr);

    const auto &published = quantity->complex_type().implicit_conversions();
    REQUIRE(published.size() == 1);
    REQUIRE_FALSE(published[0]->has_receiver());
    REQUIRE(published[0]->args.size() == 1);
    REQUIRE(published[0]->get_return_type() == quantity->value_type());

    const ValueType t_int32 = EchoTests::prim(ValueTypePrimitive::t_int32);

    // the destination owns the conversion; int32 still declares nothing
    REQUIRE(find_implicit_conversion(t_int32, quantity->value_type()) == published[0]);
    REQUIRE(find_implicit_conversion(quantity->value_type(), t_int32) == nullptr);
    REQUIRE(find_inbound_implicit_conversion(&quantity->complex_type(), t_int32) == published[0]);
}

TEST_CASE("a class may declare an inbound conversion that allocates", "[implicit_conversion]")
{
    // outbound still refuses a return that needs destruction. inbound constructs the destination,
    // so a class Quantity is the shape W1 asked for
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class Quantity {\n"
        "    int64 $n;\n"
        "    #[implicit]\n"
        "    static function from(int32 $n) : Quantity { return Quantity($n); }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *quantity = type_named(m, "Quantity");
    REQUIRE(quantity != nullptr);
    REQUIRE(quantity->complex_type().implicit_conversions().size() == 1);
    REQUIRE(find_implicit_conversion(
        EchoTests::prim(ValueTypePrimitive::t_int32), quantity->value_type()) != nullptr);
}

TEST_CASE("outbound wins the scan when both directions exist for one pair", "[implicit_conversion]")
{
    // source walk first: Buffer already knows how to become Window, so Window's inbound from
    // Buffer is not the one find_implicit_conversion answers. swapping the arms would pick
    // Window::from and every existing outbound golden would still pass
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Buffer {\n"
        "    usize $n;\n"
        "    #[implicit]\n"
        "    function window() : Window { return Window($this->n); }\n"
        "}\n"
        "struct Window {\n"
        "    usize $n;\n"
        "    #[implicit]\n"
        "    static function from(Buffer $b) : Window { return Window($b->n); }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *buffer = type_named(m, "Buffer");
    auto *window = type_named(m, "Window");
    REQUIRE(buffer != nullptr);
    REQUIRE(window != nullptr);

    FunctionDeclNode *found = find_implicit_conversion(buffer->value_type(), window->value_type());
    REQUIRE(found != nullptr);
    REQUIRE(found->has_receiver());
    REQUIRE(found->name_token.has_value());
    REQUIRE(found->name_token.value().value() == "window");

    REQUIRE(find_inbound_implicit_conversion(&window->complex_type(), buffer->value_type()) != nullptr);
}
