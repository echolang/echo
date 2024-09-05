#include "Parser/ExternParser.h"

#include "Parser/FuncDeclParser.h"

std::vector<AST::FunctionDeclNode *> Parser::parse_extern_block(Parser::Payload &payload, bool symbol_only)
{
    auto &cursor = payload.cursor;

    std::vector<AST::FunctionDeclNode *> declarations;

    if (!cursor.is_type(Token::Type::t_extern)) {
        payload.collect_unexpected_token(Token::Type::t_extern);
        cursor.try_skip_to_next_statement();
        return declarations;
    }

    // skip the extern keyword
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        cursor.try_skip_to_next_statement();
        return declarations;
    }

    // skip the opening brace
    cursor.skip();

    while (!cursor.is_type(Token::Type::t_close_brace)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(cursor.current()),
                Token::Type::t_close_brace,
                Token::Type::t_unknown);
            return declarations;
        }

        // only function declarations live in here. anything else is reported and skipped rather
        // than aborting the block, so one typo does not swallow the remaining declarations
        if (!cursor.is_type(Token::Type::t_function)) {
            payload.collect_unexpected_token(Token::Type::t_function);
            cursor.skip();
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_close_brace });
            if (cursor.is_type(Token::Type::t_semicolon)) {
                cursor.skip();
            }
            continue;
        }

        if (auto *funcdecl = parse_funcdecl(payload, symbol_only, FuncDeclKind::t_extern)) {
            declarations.push_back(funcdecl);
        }
    }

    // skip the closing brace
    cursor.skip();

    return declarations;
}
