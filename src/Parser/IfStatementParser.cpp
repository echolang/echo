#include "Parser/IfStatementParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

bool is_ifstatement_start_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_if) || cursor.is_type(Token::Type::t_else);
}

AST::IfStatementNode::Block parse_ifstatement_block(Parser::Payload &payload)
{
    // if the first token is an "if" we need to parse the condition
    AST::ExprNode *condition = nullptr;
    AST::ScopeNode *scope = nullptr;

    if (payload.cursor.is_type(Token::Type::t_if)) {
        payload.cursor.skip();

        condition = parse_expr(payload);

        if (!condition) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_unknown, payload.cursor.current().type());
            payload.cursor.try_skip_to_next_statement();
            return {condition, scope};
        }
    }

    // a opening brace is required to start the scope
    if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_open_brace, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return {condition, scope};
    }

    payload.cursor.skip();

    // parse the scope
    scope = &parse_scope(payload);

    return {condition, scope};
}

AST::IfStatementNode *Parser::parse_ifstatement(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_if)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_function, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &ifstatement = payload.context.emplace_node<AST::IfStatementNode>();

    // parse the first if statement
    ifstatement.blocks.push_back(parse_ifstatement_block(payload));

    while (payload.cursor.is_type(Token::Type::t_else)) {
        payload.cursor.skip();
        ifstatement.blocks.push_back(parse_ifstatement_block(payload));
    }

    return &ifstatement;
}
