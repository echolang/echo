#include "Parser/FuncCallParser.h"
#include "Parser/ExprParser.h"

AST::FunctionCallExprNode *Parser::parse_funccall(Parser::Payload &payload)
{
    if (!payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_paren})) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_identifier, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto funcname_token = payload.cursor.current();

    // skip the function name
    payload.cursor.skip();

    // skip the open parenthesis
    payload.cursor.skip();

    // parse the arguments
    std::vector<AST::ExprNode *> args;
    while (!payload.cursor.is_type(Token::Type::t_close_paren)) {
        if (payload.cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(funcname_token), Token::Type::t_close_paren, Token::Type::t_unknown);
            payload.cursor.try_skip_to_next_statement();
            return nullptr;
        }

        auto arg = parse_expr(payload);
        args.push_back(arg);

        if (payload.cursor.is_type(Token::Type::t_comma)) {
            payload.cursor.skip();
        }
    }

    // skip the close parenthesis
    payload.cursor.skip();

    auto &funcall = payload.context.emplace_node<AST::FunctionCallExprNode>(funcname_token, args);
    
    return &funcall;
}