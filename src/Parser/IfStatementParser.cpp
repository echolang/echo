#include "Parser/IfStatementParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

bool is_ifstatement_start_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_if) || cursor.is_type(Token::Type::t_else);
}


AST::IfStatementNode *Parser::parse_ifstatement(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_if)) {
        payload.collect_unexpected_token(Token::Type::t_function);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &ifstatement = payload.context.emplace_node<AST::IfStatementNode>();

    // parse the condition
    payload.cursor.skip(); // skip the "if" token

    ifstatement.condition = parse_expr(payload);

    if (!ifstatement.condition) {
        payload.collect_unexpected_token(Token::Type::t_unknown);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // as long as we do not support one line if statements we need to have a scope
    if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the opening brace

    ifstatement.if_scope = &parse_scope(payload);

    // expect a closing brace
    if (!payload.cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    payload.cursor.skip(); // skip the closing brace

    // parse the else block
    if (payload.cursor.is_type(Token::Type::t_else)) {
        payload.cursor.skip(); // skip the "else" token

        // if the first token is not an if keyword we expect the token to be an opening brace
        // and skip it
        auto is_end_else = !payload.cursor.is_type(Token::Type::t_if);
        if (is_end_else) {
            if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
                payload.collect_unexpected_token(Token::Type::t_open_brace);
                payload.cursor.try_skip_to_next_statement();
                return nullptr;
            }

            payload.cursor.skip(); // skip the opening brace
        }

        // parse the else scope
        // if there is another if statement aka "else if" it should automatically be parsed as a new if statement
        // instead of an else block building the tree
        ifstatement.else_scope = &parse_scope(payload);

        // expect a closing brace
        if (is_end_else) {
            if (!payload.cursor.is_type(Token::Type::t_close_brace)) {
                payload.collect_unexpected_token(Token::Type::t_close_brace);
                payload.cursor.try_skip_to_next_statement();
                return nullptr;
            }

            payload.cursor.skip(); // skip the closing brace
        }
    }

    return &ifstatement;
}
