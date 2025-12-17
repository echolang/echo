#include "Parser/AttributeParser.h"

#include "Parser/ExprParser.h"

#include "AST/ASTAttributes.h"
#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"

#include <fmt/core.h>

std::optional<std::string> Parser::attribute_string_value(
    Parser::Payload &payload,
    AST::AttributeNode *attribute,
    const std::string &attribute_name
)
{
    if (attribute->attribute_exprs.size() != 1) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(attribute->attribute_tokens),
            fmt::format("The '{}' attribute takes exactly one value.", attribute_name));
        return std::nullopt;
    }

    // AST::literal_string_value, not a hand-rolled node-type test: it is the one answer to "what text
    // does this expression spell", shared with the abort message the type checker validates and the one
    // ExprCodegen folds - so an attribute can never read a literal differently from the rest of the
    // compiler
    const AST::NodeReference &written = attribute->attribute_exprs[0];

    std::optional<std::string> value;
    if (written.has() && written.is_expression_node()) {
        value = AST::literal_string_value(written.unsafe_ptr<AST::ExprNode>());
    }

    if (!value.has_value()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(attribute->attribute_tokens),
            fmt::format("The '{}' attribute value must be a string.", attribute_name));
        return std::nullopt;
    }

    return value;
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

    // `#[name:$value]` now lexes its `:$` as the pointer-of operator - this is the one place in
    // the grammar where a colon can be immediately followed by a `$`. a space disambiguates,
    // and saying so beats letting the attribute fail as a mysteriously missing colon
    if (payload.cursor.is_type(Token::Type::t_ptr_of)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(payload.cursor.current()),
            "Write a space after ':' in an attribute value - ':$' is the pointer-of operator"
        );
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // otherwise we expect a colon and a value
    if (!payload.cursor.is_type(Token::Type::t_colon)) {
        payload.collect_unexpected_token(Token::Type::t_colon);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the colon

    // use the expression parser to parse the value
    AST::NodeReferenceList exprs;
    exprs.push_back(Parser::parse_expr_ref(payload));

    // build the attribute node
    auto &node = payload.context.emplace_node<AST::AttributeNode>(payload.cursor.slice(att_token_start, payload.cursor.snapshot()), name_token);
    node.attribute_exprs = std::move(exprs);

    // next we expect a closing square bracket
    if (!payload.cursor.is_type(Token::Type::t_close_bracket)) {
        payload.collect_unexpected_token(Token::Type::t_close_bracket);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the closing square bracket

    // attach the attribute node to the current scope
    payload.context.scope().add_attribute(node);

    return &node;
}
