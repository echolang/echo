#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAttributeReader.h>
#include <AST/ASTAttributeValue.h>
#include <AST/AttributeNode.h>
#include <Parser/AttributeParser.h>
#include <Parser/AttributeValueParser.h>

#include "helpers.h"

// what may sit after the colon in an attribute, and what a consumer may then ask of it.
//
// the grammar is a closed data grammar rather than the expression parser, and the reason is testable
// three ways over: a bare name here means itself where the expression parser would make a constant
// reference of it, a `name atom` pair is a tag where that one refuses two expressions with no operator
// between them, and `{` is a record where it is not an expression token at all.

namespace
{

using AST::AttributeValueKind;

// the value out of `#[<spelled>]`, parsed through the real attribute parser so the lookahead, the
// recovery and the scope registration are the ones a source file gets
struct ParsedAttribute
{
    std::unique_ptr<AST::Bundle> bundle;
    AST::AttributeNode *node = nullptr;

    const AST::AttributeValue &value() const {
        REQUIRE(node != nullptr);
        REQUIRE(node->value.has_value());
        return node->value.value();
    }
};

ParsedAttribute attribute_of(const std::string &spelled)
{
    // `#[inline]` is a real declaration attribute, so the attribute the case is about attaches to a
    // function and the parser walks the ordinary path to it
    ParsedAttribute parsed;
    parsed.bundle = EchoTests::tests_make_parsed_bundle(
        "#[" + spelled + "]\nfunction marked() : void {}\n");

    for (auto &module : parsed.bundle->modules) {
        for (AST::AttributeNode *attribute : module->nodes.of_type<AST::AttributeNode>()) {
            parsed.node = attribute;
        }
    }

    return parsed;
}

std::string spelling_of(const std::string &spelled)
{
    ParsedAttribute parsed = attribute_of(spelled);
    return AST::attribute_value_spelling(parsed.value());
}

};

TEST_CASE("a bare name means itself, not a constant", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("core: array");

    REQUIRE(parsed.value().kind == AttributeValueKind::t_name);
    REQUIRE(parsed.value().text == "array");
    REQUIRE_FALSE(parsed.value().has_tag());
}

TEST_CASE("a name followed by a value is a tag", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("link: framework \"OpenGL\"");

    REQUIRE(parsed.value().has_tag());
    REQUIRE(parsed.value().tag() == "framework");
    REQUIRE(parsed.value().kind == AttributeValueKind::t_string);
    REQUIRE(parsed.value().text == "OpenGL");
}

TEST_CASE("a string may be a tag, so a package name stays free text", "[attributes]")
{
    // `#[requires: "libcurl" { version:, git: }]` - a hyphenated name is not an identifier
    ParsedAttribute parsed = attribute_of(
        "core: \"lib-curl\" { version: \"^0.1\", git: \"https://x\" }");

    REQUIRE(parsed.value().has_tag());
    REQUIRE(parsed.value().tag() == "lib-curl");
    REQUIRE(parsed.value().kind == AttributeValueKind::t_record);
    REQUIRE(parsed.value().find_field("version") != nullptr);
    REQUIRE(AST::attribute_value_spelling(parsed.value()).find("\"lib-curl\"") != std::string::npos);
}

TEST_CASE("one token of lookahead is what separates a tag from a name", "[attributes]")
{
    // `array` is followed by the `]` that closes the attribute, which starts no value - so it is a
    // name. this is the whole of the ambiguity, and getting it wrong makes every `#[core:]` a tag
    REQUIRE_FALSE(attribute_of("core: array").value().has_tag());
    REQUIRE(attribute_of("link: lib \"m\"").value().has_tag());
}

TEST_CASE("a string is decoded, not carried verbatim", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("intrinsic: \"a\\tb\"");

    REQUIRE(parsed.value().kind == AttributeValueKind::t_string);
    REQUIRE(parsed.value().text == "a\tb");
}

TEST_CASE("numbers, bools and a negated number", "[attributes]")
{
    REQUIRE(attribute_of("core: 42").value().integer == 42);
    REQUIRE(attribute_of("core: 0x2a").value().integer == 42);
    REQUIRE(attribute_of("core: 0b101010").value().integer == 42);
    REQUIRE(attribute_of("core: -7").value().integer == -7);
    REQUIRE(attribute_of("core: true").value().boolean == true);
    REQUIRE(attribute_of("core: 1.5").value().kind == AttributeValueKind::t_float);
}

TEST_CASE("a list holds values, and may end on a comma", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("sources: [\"a/*.eco\", \"b/*.eco\",]");

    REQUIRE(parsed.value().kind == AttributeValueKind::t_list);
    REQUIRE(parsed.value().items.size() == 2);
    REQUIRE(parsed.value().items[1].text == "b/*.eco");
}

TEST_CASE("a record keeps its keys in written order", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("depends: git { url: \"https://x\", rev: \"v1\" }");

    REQUIRE(parsed.value().kind == AttributeValueKind::t_record);
    REQUIRE(parsed.value().tag() == "git");
    REQUIRE(parsed.value().fields.size() == 2);
    REQUIRE(parsed.value().fields[0].key == "url");
    REQUIRE(parsed.value().fields[1].key == "rev");
    REQUIRE(parsed.value().find_field("rev") != nullptr);
    REQUIRE(parsed.value().find_field("branch") == nullptr);
}

TEST_CASE("a record nests a list, and a list nests a record", "[attributes]")
{
    ParsedAttribute nested = attribute_of("cc: define { NAMES: [\"a\", \"b\"] }");

    REQUIRE(nested.value().fields.size() == 1);
    REQUIRE(nested.value().fields[0].value.kind == AttributeValueKind::t_list);
    REQUIRE(nested.value().fields[0].value.items.size() == 2);
}

TEST_CASE("a value spells itself back", "[attributes]")
{
    // read by the `-a` dump and by every refusal that quotes a value, so it has to survive a round trip
    REQUIRE(spelling_of("link: lib \"GL\"") == "lib \"GL\"");
    REQUIRE(spelling_of("core: array") == "array");
    REQUIRE(spelling_of("sources: [\"a\", \"b\"]") == "[\"a\", \"b\"]");
    REQUIRE(spelling_of("cc: define { A: 1 }") == "define { A: 1 }");
}

TEST_CASE("a flag attribute carries no value at all", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("inline");

    REQUIRE(parsed.node != nullptr);
    REQUIRE_FALSE(parsed.node->value.has_value());
    REQUIRE(parsed.node->node_description() == "attr<inline>");
}

TEST_CASE("a malformed value is reported and costs one message", "[attributes]")
{
    // the recovery is the balanced `[ ... ]` rather than a hunt for a statement terminator, so the
    // declaration after a broken attribute still parses
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[core: { 1: \"x\" }]\nstruct Fine { int32 $a; }\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

TEST_CASE("a duplicate record key is refused at the second one", "[attributes]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[depends: git { url: \"a\", url: \"b\" }]\necho 1;\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

TEST_CASE("one value reads as a list of one", "[attributes]")
{
    // **anywhere one value is accepted, a list of them is**, and it is one rule rather than an arm per
    // attribute - so no consumer has to know which spelling it was handed
    ParsedAttribute single = attribute_of("sources: \"a\"");
    ParsedAttribute several = attribute_of("sources: [\"a\", \"b\"]");

    REQUIRE(AST::AttributeReader::each(single.value()).size() == 1);
    REQUIRE(AST::AttributeReader::each(several.value()).size() == 2);
}

TEST_CASE("a tagged list is one tagged thing, until its tag is taken", "[attributes]")
{
    // the two peels answer different questions: `each` must not split `lib ["GL", "GLU"]` into two
    // values with no scheme between them, and `payload` must, once the scheme is the caller's
    ParsedAttribute parsed = attribute_of("link: lib [\"GL\", \"GLU\"]");

    REQUIRE(AST::AttributeReader::each(parsed.value()).size() == 1);
    REQUIRE(AST::AttributeReader::payload(parsed.value()).size() == 2);
}

TEST_CASE("the reader accumulates refusals rather than reporting them", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("core: \"array\"");

    AST::AttributeReader reader("core");
    REQUIRE_FALSE(reader.name(parsed.value()).has_value());
    REQUIRE(reader.has_refusals());

    // the span is the value's, not the whole attribute's - which is what lets a message underline the
    // thing that is wrong rather than the line it sits on
    REQUIRE(reader.refusals().front().span.is_valid());
    REQUIRE(reader.refusals().front().message.find("wants a name") != std::string::npos);
}

TEST_CASE("an unknown record key is refused, a known one is not", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("depends: git { url: \"a\", branch: \"main\" }");

    AST::AttributeReader reader("depends");
    reader.reject_unknown_fields(parsed.value(), { "url", "rev" });

    REQUIRE(reader.refusals().size() == 1);
    REQUIRE(reader.refusals().front().message.find("'branch' is not something") != std::string::npos);
}

TEST_CASE("a missing required field is refused by name", "[attributes]")
{
    ParsedAttribute parsed = attribute_of("depends: git { rev: \"v1\" }");

    AST::AttributeReader reader("depends");
    REQUIRE(reader.require_field(parsed.value(), "url") == nullptr);
    REQUIRE(reader.refusals().front().message.find("missing its 'url'") != std::string::npos);
}

TEST_CASE("a word reads a name, a string or a number alike", "[attributes]")
{
    // what a `define` record's values go through: `{ N: 40 }`, `{ N: "40" }` and `{ N: OTHER }` all
    // mean one word on a command line
    ParsedAttribute parsed = attribute_of("cc: define { A: 1, B: \"x\", C: OTHER, D: true }");

    AST::AttributeReader reader("cc");
    REQUIRE(reader.word(parsed.value().fields[0].value) == "1");
    REQUIRE(reader.word(parsed.value().fields[1].value) == "x");
    REQUIRE(reader.word(parsed.value().fields[2].value) == "OTHER");

    // a bool is the one refusal, because C has no spelling for one
    REQUIRE_FALSE(reader.word(parsed.value().fields[3].value).has_value());
}
