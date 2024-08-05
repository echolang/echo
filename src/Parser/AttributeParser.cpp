#include "Parser/AttributeParser.h"

#include "Parser/ExprParser.h"

AST::AttributeNode *Parser::parse_attribute(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_hash)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_hash, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the attribute keyword

    // now we except an opening square bracket "["
    if (!payload.cursor.is_type(Token::Type::t_open_bracket)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_open_bracket, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the opening square bracket

    // now we expect an identifier
    if (!payload.cursor.is_type(Token::Type::t_identifier)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_identifier, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto name_token = payload.cursor.current();
    auto att_token_start = payload.cursor.snapshot();
    payload.cursor.skip(); // skip the identifier


    // if the next token is a closing square bracket, then we have a simple attribute
    if (payload.cursor.is_type(Token::Type::t_close_bracket)) {
        payload.cursor.skip(); // skip the closing square bracket

        // create the attribute node
        return &payload.context.emplace_node<AST::AttributeNode>(payload.cursor.slice(att_token_start, payload.cursor.snapshot()), name_token);
    }

    // otherwise we expect a colon and a value
    if (!payload.cursor.is_type(Token::Type::t_colon)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_colon, payload.cursor.current().type());
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
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_close_bracket, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the closing square bracket

    // attach the attribute node to the current scope
    payload.context.scope().add_attribute(node);

    return &node;
}