#include "Parser/WhileStatementParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

AST::WhileStatementNode *Parser::parse_whilestatement(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_while)) {
        payload.collect_unexpected_token(Token::Type::t_while);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &whilestmt = payload.context.emplace_node<AST::WhileStatementNode>();

    // parse the condition
    payload.cursor.skip(); // skip the "while" token

    whilestmt.condition = parse_expr(payload);

    if (!whilestmt.condition) {
        payload.collect_unexpected_token(Token::Type::t_unknown);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto loop_brace = payload.cursor.current();
    payload.cursor.skip(); // skip the opening brace

    {
        // **the body only.** the condition above is parsed outside this guard on purpose: a `break`
        // written in a loop's condition belongs to an enclosing loop, not to this one. it wraps the whole
        // parse_scope call so an `if` arm, a `guard`'s else arm and a nested block inside the body all
        // inherit the depth
        AST::LoopScope loop(payload.context, payload.context.loop_depth + 1);

        whilestmt.loop_scope = &parse_scope(payload, loop_brace);
    }

    // expect a closing brace
    if (!payload.cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the closing brace

    return &whilestmt;
}
