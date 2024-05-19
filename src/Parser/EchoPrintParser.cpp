#include "Parser/EchoPrintParser.h"

#include "Parser/ExprParser.h"

AST::FunctionCallExprNode * Parser::parse_echo(Payload &payload)
{
    auto echo_token = payload.cursor.current();

    if (echo_token.type() != Token::Type::t_echo) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(echo_token), Token::Type::t_echo, echo_token.type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip();

    auto expr = parse_expr(payload);

    if (expr == nullptr) {
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    std::vector<AST::ExprNode *> args = { expr };

    auto &node = payload.context.emplace_node<AST::FunctionCallExprNode>(echo_token, args);

    // next token should be a semicolon
    if (payload.cursor.current().type() != Token::Type::t_semicolon) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_semicolon, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
    } else {
        payload.cursor.skip();
    }

    return &node;
}
