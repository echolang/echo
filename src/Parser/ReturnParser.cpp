#include "Parser/ReturnParser.h"

#include "Parser/ExprParser.h"

AST::ReturnNode &Parser::parse_return(Parser::Payload &payload)
{
    // sanity check that the current token is a return keyword
    if (!payload.cursor.is_type(Token::Type::t_return)) {
        payload.collect_unexpected_token(Token::Type::t_return);
        payload.cursor.try_skip_to_next_statement();
        auto &expr = payload.context.emplace_node<AST::VoidExprNode>();

        return payload.context.emplace_node<AST::ReturnNode>(&expr);
    }

    auto return_token = payload.cursor.current();

    // skip the return keyword
    payload.cursor.skip();

    // parse the expression that follows the return keyword
    auto expr = parse_expr(payload);

    // ensure we have a semicolon at the end of the return statement
    if (!payload.cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        payload.cursor.try_skip_to_next_statement();
    }
    else {
        payload.cursor.skip();
    }

    return payload.context.emplace_node<AST::ReturnNode>(expr, return_token);
}