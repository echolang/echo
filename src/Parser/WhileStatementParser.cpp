#include "Parser/WhileStatementParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

AST::WhileStatementNode *Parser::parse_whilestatement(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_while)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_while, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &whilestmt = payload.context.emplace_node<AST::WhileStatementNode>();

    // parse the condition
    payload.cursor.skip(); // skip the "while" token

    whilestmt.condition = parse_expr(payload);

    if (!whilestmt.condition) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_unknown, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_open_brace, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the opening brace

    whilestmt.loop_scope = &parse_scope(payload);

    // expect a closing brace
    if (!payload.cursor.is_type(Token::Type::t_close_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_close_brace, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the closing brace

    return &whilestmt;
}
