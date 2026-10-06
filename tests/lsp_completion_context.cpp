#include <catch2/catch_test_macros.hpp>

#include <Compiler/Lsp/LspCompletionContext.h>

#include <string>
#include <vector>

namespace
{
    using Compiler::Lsp::CompletionKind;
    using Compiler::Lsp::NamePosition;
    using Compiler::Lsp::ReceiverSegment;

    // `|` marks the cursor and is stripped from the text
    Compiler::Lsp::CompletionContext context_at(const std::string &marked)
    {
        const size_t cursor = marked.find('|');
        REQUIRE(cursor != std::string::npos);
        std::string text = marked;
        text.erase(cursor, 1);
        return Compiler::Lsp::analyze_completion_context(text, cursor);
    }

    std::vector<std::string> names_of(const std::vector<Compiler::Lsp::LexicalVariable> &variables)
    {
        std::vector<std::string> out;
        for (const auto &variable : variables) {
            out.push_back(variable.name);
        }
        return out;
    }
};

TEST_CASE("completion context: a variable prefix includes its sigil", "[lsp]")
{
    const auto context = context_at("function f() : void { $co| }");
    REQUIRE(context.kind == CompletionKind::t_variable);
    REQUIRE(context.prefix == "$co");
    REQUIRE(context.replace_start == std::string("function f() : void { ").size());
}

TEST_CASE("completion context: the identifier after the cursor is part of what gets replaced", "[lsp]")
{
    const std::string text = "function f() : void { $count = 1; }";
    const size_t cursor = text.find("ount");
    const auto context = Compiler::Lsp::analyze_completion_context(text, cursor);
    REQUIRE(context.prefix == "$c");
    REQUIRE(context.replace_end == text.find(" = 1"));
}

TEST_CASE("completion context: member access reads the receiver chain", "[lsp]")
{
    auto context = context_at("function f() : void { $p->na| }");
    REQUIRE(context.kind == CompletionKind::t_member);
    REQUIRE(context.prefix == "na");
    REQUIRE(context.receiver.size() == 1);
    REQUIRE(context.receiver[0].kind == ReceiverSegment::Kind::t_variable);
    REQUIRE(context.receiver[0].name == "$p");

    context = context_at("$a->b()->c[0]->|");
    REQUIRE(context.kind == CompletionKind::t_member);
    REQUIRE(context.receiver.size() == 4);
    REQUIRE(context.receiver[1].kind == ReceiverSegment::Kind::t_method);
    REQUIRE(context.receiver[1].name == "b");
    REQUIRE(context.receiver[2].kind == ReceiverSegment::Kind::t_property);
    REQUIRE(context.receiver[3].kind == ReceiverSegment::Kind::t_index);

    context = context_at("$a?->|");
    REQUIRE(context.kind == CompletionKind::t_member);
    REQUIRE(context.optional_chain);

    context = context_at("geo::make(1, f(2))->|");
    REQUIRE(context.receiver.size() == 1);
    REQUIRE(context.receiver[0].kind == ReceiverSegment::Kind::t_static_call);
    REQUIRE(context.receiver[0].name == "make");
    REQUIRE(context.receiver[0].path == std::vector<std::string>{ "geo" });

    context = context_at("Point(1, 2)->|");
    REQUIRE(context.receiver[0].kind == ReceiverSegment::Kind::t_call);
}

TEST_CASE("completion context: a static path and a use path", "[lsp]")
{
    auto context = context_at("echo std::math::sq|");
    REQUIRE(context.kind == CompletionKind::t_static);
    REQUIRE(context.path == std::vector<std::string>{ "std", "math" });
    REQUIRE(context.prefix == "sq");

    context = context_at("echo Counter::$|");
    REQUIRE(context.kind == CompletionKind::t_static);
    REQUIRE(context.prefix == "$");

    context = context_at("use std::|");
    REQUIRE(context.kind == CompletionKind::t_use_path);
    REQUIRE(context.path == std::vector<std::string>{ "std" });

    context = context_at("use std::math::{sqrt, |");
    REQUIRE(context.kind == CompletionKind::t_use_path);
    REQUIRE(context.path == std::vector<std::string>{ "std", "math" });
}

TEST_CASE("completion context: shorthand names its destination", "[lsp]")
{
    auto context = context_at("function f() : void { DistanceUnit $u = .|");
    REQUIRE(context.kind == CompletionKind::t_shorthand);
    REQUIRE(context.destination_type == "DistanceUnit");

    context = context_at("function f() : Unit { return .|");
    REQUIRE(context.kind == CompletionKind::t_shorthand);
    REQUIRE(context.destination_is_return);

    context = context_at("function f() : void { if ($u == .ki| ");
    REQUIRE(context.kind == CompletionKind::t_shorthand);
    REQUIRE(context.destination_chain.size() == 1);
    REQUIRE(context.destination_chain[0].name == "$u");
}

TEST_CASE("completion context: nothing in comments, strings, numbers and ranges", "[lsp]")
{
    REQUIRE(context_at("// $|").kind == CompletionKind::t_none);
    REQUIRE(context_at("/* $p->| */").kind == CompletionKind::t_none);
    REQUIRE(context_at("echo \"$|").kind == CompletionKind::t_none);
    REQUIRE(context_at("echo 'abc|").kind == CompletionKind::t_none);
    REQUIRE(context_at("$x = 3.|").kind == CompletionKind::t_none);
    REQUIRE(context_at("foreach (0 ..|").kind == CompletionKind::t_none);
    REQUIRE(context_at("#[if: |").kind == CompletionKind::t_none);
    REQUIRE(context_at("$x = 12|").kind == CompletionKind::t_none);
}

TEST_CASE("completion context: an interpolation hole is code", "[lsp]")
{
    const auto context = context_at("echo \"total: {$to|");
    REQUIRE(context.kind == CompletionKind::t_variable);
    REQUIRE(context.prefix == "$to");

    // and the string resumes after it
    REQUIRE(context_at("echo \"{$a} and $|").kind == CompletionKind::t_none);
}

TEST_CASE("completion context: attributes and bare names", "[lsp]")
{
    REQUIRE(context_at("#[inl|").kind == CompletionKind::t_attribute);

    auto context = context_at("fo|");
    REQUIRE(context.kind == CompletionKind::t_identifier);
    REQUIRE(context.position == NamePosition::t_file_root);

    context = context_at("function f() : void { fo|");
    REQUIRE(context.position == NamePosition::t_statement);

    context = context_at("struct P { con|");
    REQUIRE(context.position == NamePosition::t_type_body);

    context = context_at("function f() : |");
    REQUIRE(context.position == NamePosition::t_type);

    context = context_at("function f(|");
    REQUIRE(context.position == NamePosition::t_type);

    context = context_at("function f() : void { $x = |");
    REQUIRE(context.position == NamePosition::t_expression);

    context = context_at("function f() : void { $x = $y |");
    REQUIRE(context.position == NamePosition::t_after_expression);
}

TEST_CASE("completion context: visible variables follow scope", "[lsp]")
{
    auto context = context_at(
        "$outer = 1;\n"
        "function f(int32 $a, string $b) : void {\n"
        "    int32 $n = 1;\n"
        "    { $inner = 2; }\n"
        "    foreach ($items as $k => $v) {\n"
        "        $|\n");
    REQUIRE(context.kind == CompletionKind::t_variable);
    REQUIRE(names_of(Compiler::Lsp::visible_variables(context))
        == std::vector<std::string>{ "$k", "$v", "$a", "$b", "$n" });

    // the parameter carries its written type
    const auto variables = Compiler::Lsp::visible_variables(context);
    REQUIRE(variables[2].written_type == "int32");
    REQUIRE(variables[2].is_parameter);
}

TEST_CASE("completion context: a closure sees what it captures; a function stops the walk", "[lsp]")
{
    auto context = context_at(
        "function f() : void {\n"
        "    int32 $n = 5;\n"
        "    function<int32()> $read = function(int32 $x) : int32 { return $|");
    REQUIRE(names_of(Compiler::Lsp::visible_variables(context))
        == std::vector<std::string>{ "$x", "$n", "$read" });

    context = context_at("$top = 1;\n$|");
    REQUIRE(names_of(Compiler::Lsp::visible_variables(context)) == std::vector<std::string>{ "$top" });
}

TEST_CASE("completion context: a method has $this; a static method has none", "[lsp]")
{
    auto context = context_at("class Gate { int32 $id; function open() : void { $| } }");
    const auto *owner = Compiler::Lsp::this_frame(context);
    REQUIRE(owner != nullptr);
    REQUIRE(owner->name == "Gate");

    // the property is a field: `$` names locals and parameters
    REQUIRE(Compiler::Lsp::visible_variables(context).empty());

    context = context_at("class Gate { static function make() : Gate { $| } }");
    REQUIRE(Compiler::Lsp::this_frame(context) == nullptr);
    REQUIRE(Compiler::Lsp::enclosing_type_frame(context) != nullptr);
}

TEST_CASE("completion context: typed declarations keep their written type", "[lsp]")
{
    const auto context = context_at(
        "function f() : void {\n"
        "    array<rc<Point>> $points = [];\n"
        "    geo::Point? $maybe = null;\n"
        "    $made = Point(1, 2);\n"
        "    $|\n");
    const auto variables = Compiler::Lsp::visible_variables(context);
    REQUIRE(variables.size() == 3);
    REQUIRE(variables[0].written_type == "array<rc<Point>>");
    REQUIRE(variables[1].written_type == "geo::Point?");
    REQUIRE(variables[2].written_type.empty());
    REQUIRE(Compiler::Lsp::receiver_chain_of(variables[2].initializer).size() == 1);
}
