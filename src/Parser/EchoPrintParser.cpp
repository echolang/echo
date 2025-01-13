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

    // AST::is_print_call is what this node answers true to, and settling it here is the one thing
    // that recognition cannot be derived from: there is nothing for AST::CallResolver to look up, and
    // nothing to fit to a parameter list that does not exist
    //
    // sayable only because settlement is its own state rather than "decl is set": with the state
    // inferred from the pointer, a node like this could not tell the finalizing sweep it was finished
    node.settlement = AST::CallSettlement::t_settled;

    // next token should be a semicolon
    if (payload.cursor.current().type() != Token::Type::t_semicolon) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        payload.cursor.try_skip_to_next_statement();
    } else {
        payload.cursor.skip();
    }

    return &node;
}
