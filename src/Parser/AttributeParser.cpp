#include "Parser/AttributeParser.h"

#include "Parser/AttributeValueParser.h"

#include "AST/ASTAttributes.h"

#include <fmt/core.h>

void Parser::report_attribute_refusals(Parser::Payload &payload, const AST::AttributeReader &reader)
{
    for (const AST::AttributeRefusal &refusal : reader.refusals()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(refusal.span.slice()), refusal.message);
    }
}

const AST::AttributeValue *Parser::attribute_value_of(
    Parser::Payload &payload,
    AST::AttributeNode *attribute,
    const std::string &attribute_name
)
{
    if (attribute->value.has_value()) {
        return &attribute->value.value();
    }

    payload.collector.collect_issue<AST::Issue::GenericError>(
        payload.context.code_ref(attribute->attribute_tokens),
        fmt::format("The '{}' attribute needs a value - write '#[{}: ...]'.", attribute_name, attribute_name));

    return nullptr;
}

AST::AttributeNode *Parser::parse_attribute(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_hash)) {
        payload.collect_unexpected_token(Token::Type::t_hash);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the attribute keyword

    // now we except an opening square bracket "["
    if (!payload.cursor.is_type(Token::Type::t_open_bracket)) {
        payload.collect_unexpected_token(Token::Type::t_open_bracket);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // kept so a refused value can be stepped over as the balanced group it is, rather than recovered
    // from by hunting for the next statement terminator - an attribute is not a statement and the `]`
    // that ends it is right there
    const Parser::Cursor::Snapshot bracket_start = payload.cursor.snapshot();

    const auto step_over_the_bracket = [&]() -> AST::AttributeNode * {
        payload.cursor.restore(bracket_start);
        payload.cursor.skip_balanced_group(Token::Type::t_open_bracket, Token::Type::t_close_bracket);
        return nullptr;
    };

    payload.cursor.skip(); // skip the opening square bracket

    // now we expect an identifier
    if (!payload.cursor.is_type(Token::Type::t_identifier)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto name_token = payload.cursor.current();

    // **an unknown attribute is refused here**, at the name, rather than accepted and left for a consumer
    // that will never come looking. Until this check existed, `#[bultin: "size_of"]` parsed, attached and
    // did nothing - leaving a bodyless function with no implementation and no diagnostic anywhere.
    //
    // reported and then *skipped past* rather than returned as null, so that the declaration after it still
    // parses: one misspelled attribute should cost one message, not the whole file
    if (!payload.is_manifest && !AST::is_known_attribute(name_token.value())) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            "unknown attribute '" + name_token.value() + "', expected one of: "
                + AST::known_attribute_list());
    }

    auto att_token_start = payload.cursor.snapshot();
    payload.cursor.skip(); // skip the identifier

    // if the next token is a closing square bracket, then we have a simple attribute
    if (payload.cursor.is_type(Token::Type::t_close_bracket)) {
        payload.cursor.skip(); // skip the closing square bracket

        // create the attribute node and register it on the scope, exactly like the valued form
        // below. without the registration a valueless attribute parsed and was then dropped on
        // the floor, so `#[inline]` never reached the declaration that followed it
        auto &node = payload.context.emplace_node<AST::AttributeNode>(payload.cursor.slice(att_token_start, payload.cursor.snapshot()), name_token);
        payload.context.scope().add_attribute(node);

        return &node;
    }

    if (!Parser::expect_attribute_colon(payload)) {
        return step_over_the_bracket();
    }

    AST::AttributeValue value;

    if (!Parser::parse_attribute_value(payload, value)) {
        return step_over_the_bracket();
    }

    // build the attribute node
    auto &node = payload.context.emplace_node<AST::AttributeNode>(payload.cursor.slice(att_token_start, payload.cursor.snapshot()), name_token);
    node.value = std::move(value);

    // next we expect a closing square bracket
    if (!payload.cursor.is_type(Token::Type::t_close_bracket)) {
        payload.collect_unexpected_token(Token::Type::t_close_bracket);
        return step_over_the_bracket();
    }

    payload.cursor.skip(); // skip the closing square bracket

    // attach the attribute node to the current scope
    payload.context.scope().add_attribute(node);

    return &node;
}
