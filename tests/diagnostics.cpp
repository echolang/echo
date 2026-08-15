#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTDiagnostic.h>
#include <AST/ASTDiagnosticRenderer.h>
#include <AST/ASTIssue.h>
#include <Compiler/TerminalCapabilities.h>

#include <sstream>
#include <string>

#include "helpers.h"

// what the e2e corpus cannot assert. tests_eco pins the *drawn* form, which is the ascii one because the
// harness runs echoc through a pipe - so the span arithmetic underneath it, the json object and the
// capability resolution are all covered from here instead.
//
// the json is here in particular for a reason worth writing down: it carries the full path of the file
// the diagnostic is in, and the harness hands echoc an absolute one. A golden holding that would pass on
// exactly the machine that recorded it

using namespace AST;

namespace
{
    // the first diagnostic a snippet produces, flattened. every assertion below is about that shape
    // rather than about a rendered string, which is the split AST::Diagnostic exists to make possible
    Diagnostic first_diagnostic(const AST::Bundle &bundle)
    {
        REQUIRE(bundle.collector.issues.size() > 0);
        return to_diagnostic(*bundle.collector.issues[0]);
    }

    std::string render(const Diagnostic &diagnostic, Compiler::DiagnosticFormat format)
    {
        std::ostringstream out;
        DiagnosticRenderer renderer(out, format, Compiler::TerminalCapabilities::plain());
        renderer.render(diagnostic);
        return out.str();
    }
}

TEST_CASE("a span covers the whole token, not just its first character", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("echo $undefined;\n");

    const Diagnostic diagnostic = first_diagnostic(*bundle);

    REQUIRE(diagnostic.primary.start.line == 1);
    REQUIRE(diagnostic.primary.start.column == 6);

    // `$undefined` is ten characters, and the end column is one past the last of them - which is what
    // makes an underline possible at all. The renderer this replaced drew a single caret
    REQUIRE(diagnostic.primary.end.line == 1);
    REQUIRE(diagnostic.primary.end.column == 16);
}

TEST_CASE("a span reaches across several tokens", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Outer { int32 $tag; }\n"
        "function get() : Outer { return Outer(1); }\n"
        "int32& $r = &get()->tag;\n");

    const Diagnostic diagnostic = first_diagnostic(*bundle);

    REQUIRE(diagnostic.primary.start.line == 3);
    REQUIRE(diagnostic.primary.end.column > diagnostic.primary.start.column);
}

TEST_CASE("the issue class name is the diagnostic code", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("echo $undefined;\n");

    REQUIRE(first_diagnostic(*bundle).code == std::string("UnknownVariable"));
}

TEST_CASE("a generic error carries no code, because it is not a classification", "[diagnostics]")
{
    // a site this batch deliberately left unclassified - not a leftover `&5`, which is now
    // AddressOfTemporary
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f() : void { int32 $n = 1; echo $n->x; }\n");

    const Diagnostic diagnostic = first_diagnostic(*bundle);

    // an editor is told nothing rather than told that the unclassified remainder is one kind
    REQUIRE(!diagnostic.code.has_value());
}

TEST_CASE("place refusals carry an AddressOfTemporary code", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("int32 $r = &5;\n");

    REQUIRE(first_diagnostic(*bundle).code == std::string("AddressOfTemporary"));
}

TEST_CASE("mv of a temporary carries a MoveOfTemporary code", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function make() : int32 { return 1; }\n"
        "$a = mv make();\n");

    REQUIRE(first_diagnostic(*bundle).code == std::string("MoveOfTemporary"));
}

TEST_CASE("a bodyless function carries a BodylessFunction code", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("function missing() : void;\n");

    REQUIRE(first_diagnostic(*bundle).code == std::string("BodylessFunction"));
}

TEST_CASE("an owning copy refusal carries a CannotCopy code", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Own { destructor() {}\n    int32 $x;\n }\n"
        "Own $a = Own(1);\n"
        "Own $b = $a;\n");

    REQUIRE(first_diagnostic(*bundle).code == std::string("CannotCopy"));
    REQUIRE(!first_diagnostic(*bundle).notes.empty());
}

TEST_CASE("a redeclaration points at the declaration that survived", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Foo { int32 $x; }\n"
        "struct Foo { int32 $y; }\n");

    const Diagnostic diagnostic = first_diagnostic(*bundle);

    REQUIRE(diagnostic.labels.size() == 1);
    REQUIRE(diagnostic.labels[0].span.start.line == 1);

    // the location used to be a clause in the message. it is a second frame now, so the reader sees the
    // line rather than being given coordinates to go and find
    REQUIRE(diagnostic.message.find("line 1") == std::string::npos);
}

TEST_CASE("both themes draw the same information", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("echo $undefined;\n");
    const Diagnostic diagnostic = first_diagnostic(*bundle);

    const std::string ascii = render(diagnostic, Compiler::DiagnosticFormat::t_ascii);

    REQUIRE(ascii.find("[error]") != std::string::npos);
    REQUIRE(ascii.find("UnknownVariable") != std::string::npos);
    REQUIRE(ascii.find("^^^^^^^^^^") != std::string::npos);
    REQUIRE(ascii.find("echo $undefined;") != std::string::npos);

    // plain() reports no colour, so nothing may reach the stream that a log has to filter back out -
    // and the ascii theme may use nothing a Windows console cannot draw
    REQUIRE(ascii.find('\x1b') == std::string::npos);
    for (const unsigned char c : ascii) {
        REQUIRE(c < 0x80);
    }
}

TEST_CASE("no rendered line can be mistaken for a test section header", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("echo $undefined;\n");
    const std::string ascii
        = render(first_diagnostic(*bundle), Compiler::DiagnosticFormat::t_ascii);

    // `--- NAME --->` is matched as a whole line by EchoTests::parse_eco_test_file, and the banner this
    // replaced was `---- Issue ----`. Nothing may start with three dashes, or a golden truncates in
    // silence at whatever the renderer emitted
    size_t start = 0;
    while (start < ascii.size()) {
        const size_t newline = ascii.find('\n', start);
        const std::string line = ascii.substr(
            start, newline == std::string::npos ? std::string::npos : newline - start);

        REQUIRE(line.rfind("---", 0) != 0);

        if (newline == std::string::npos) {
            break;
        }

        start = newline + 1;
    }
}

TEST_CASE("the json form carries the span and the full path", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("echo $undefined;\n");
    const std::string json
        = render(first_diagnostic(*bundle), Compiler::DiagnosticFormat::t_json);

    REQUIRE(json.find("\"type\":\"diagnostic\"") != std::string::npos);
    REQUIRE(json.find("\"severity\":\"error\"") != std::string::npos);
    REQUIRE(json.find("\"code\":\"UnknownVariable\"") != std::string::npos);
    REQUIRE(json.find("\"start\":{\"line\":1,\"column\":6}") != std::string::npos);
    REQUIRE(json.find("\"end\":{\"line\":1,\"column\":16}") != std::string::npos);

    // one object per line, so a reader can consume the stream as it arrives rather than waiting for the
    // compile to finish
    REQUIRE(json.back() == '\n');
    REQUIRE(json.find('\n') == json.size() - 1);
}

TEST_CASE("a json message escapes what json cannot carry raw", "[diagnostics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int32 $a) : void {}\n"
        "function f(float64 $a) : void {}\n"
        "f(1, 2, 3);\n");

    const std::string json
        = render(first_diagnostic(*bundle), Compiler::DiagnosticFormat::t_json);

    // the overload list is a multi-line message, and a raw newline inside a json string is a parse
    // error - so this is the case that proves the writer escapes rather than concatenates
    REQUIRE(json.find("\\n") != std::string::npos);
    REQUIRE(json.find('\n') == json.size() - 1);
}

TEST_CASE("the summary is the one line a tool can wait for", "[diagnostics]")
{
    std::ostringstream out;
    DiagnosticRenderer renderer(
        out, Compiler::DiagnosticFormat::t_json, Compiler::TerminalCapabilities::plain());

    renderer.render_summary(2, 1, /*compiled=*/false);

    REQUIRE(out.str() == "{\"type\":\"summary\",\"errors\":2,\"warnings\":1,\"compiled\":false}\n");
}

TEST_CASE("a clean compile prints no tail", "[diagnostics]")
{
    std::ostringstream out;
    DiagnosticRenderer renderer(
        out, Compiler::DiagnosticFormat::t_ascii, Compiler::TerminalCapabilities::plain());

    renderer.render_summary(0, 0, /*compiled=*/true);

    REQUIRE(out.str() == "");
}

TEST_CASE("an explicit colour or format choice overrides the environment", "[diagnostics]")
{
    using Compiler::ColorChoice;
    using Compiler::DiagnosticFormat;
    using Compiler::TerminalCapabilities;

    // the suite runs with stderr on a pipe, so `auto` on both axes has to answer no. Anything else and
    // a CI log fills with escape sequences nobody asked for
    const TerminalCapabilities automatic
        = TerminalCapabilities::resolve(ColorChoice::t_auto, DiagnosticFormat::t_auto);
    REQUIRE(!automatic.color);
    REQUIRE(!automatic.unicode);

    // and no flag can make it say otherwise: `interactive` is what keeps a carriage return out of this
    // suite's captured output, and out of the 596 e2e goldens it byte-compares
    REQUIRE(!automatic.interactive);
    REQUIRE(!TerminalCapabilities::resolve(ColorChoice::t_always, DiagnosticFormat::t_pretty).interactive);
    REQUIRE(!TerminalCapabilities::plain().interactive);

    REQUIRE(TerminalCapabilities::resolve(ColorChoice::t_always, DiagnosticFormat::t_auto).color);
    REQUIRE(!TerminalCapabilities::resolve(ColorChoice::t_never, DiagnosticFormat::t_pretty).color);
    REQUIRE(TerminalCapabilities::resolve(ColorChoice::t_auto, DiagnosticFormat::t_pretty).unicode);
    REQUIRE(!TerminalCapabilities::resolve(ColorChoice::t_auto, DiagnosticFormat::t_ascii).unicode);

    // json carries no glyphs, so it must not report that it can draw one - a caller that forgot to
    // branch on the format would otherwise write a box-drawing character into a parsed stream
    REQUIRE(!TerminalCapabilities::resolve(ColorChoice::t_auto, DiagnosticFormat::t_json).unicode);
}

TEST_CASE("a mistyped colour or format is refused, never defaulted", "[diagnostics]")
{
    Compiler::ColorChoice choice = Compiler::ColorChoice::t_always;
    Compiler::DiagnosticFormat format = Compiler::DiagnosticFormat::t_pretty;
    std::string error;

    REQUIRE(Compiler::parse_color_choice("never", choice, error));
    REQUIRE(choice == Compiler::ColorChoice::t_never);

    // `--color=alwyas` silently meaning `auto` is a flag the user believes they set
    REQUIRE(!Compiler::parse_color_choice("alwyas", choice, error));
    REQUIRE(error.find("alwyas") != std::string::npos);

    REQUIRE(Compiler::parse_diagnostic_format("json", format, error));
    REQUIRE(format == Compiler::DiagnosticFormat::t_json);

    REQUIRE(!Compiler::parse_diagnostic_format("fancy", format, error));
    REQUIRE(error.find("fancy") != std::string::npos);
}
