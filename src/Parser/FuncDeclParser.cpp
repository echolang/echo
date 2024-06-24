#include "Parser/FuncDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"

void Parser::parse_funcdecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_function)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_function, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    // skip the function keyword
    cursor.skip();

    // next token should be an identifier aka the function name
    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    // fetch the function name and skip it
    auto nametoken = cursor.current();
    cursor.skip();

    // next token needs to be an open parenthesis
    if (!cursor.is_type(Token::Type::t_open_paren)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_paren, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    auto &funcdecl = payload.context.emplace_node<AST::FunctionDeclNode>(nametoken);

    // skip the open parenthesis
    cursor.skip();

    // create an empty base scope for the function and the arguments to sit in
    auto &funcscope = payload.context.emplace_node<AST::ScopeNode>();

    // parse the function arguments
    while (!cursor.is_type(Token::Type::t_close_paren)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(nametoken), Token::Type::t_close_paren, Token::Type::t_unknown);
            cursor.try_skip_to_next_statement();
            return;
        }

        auto vardecl = parse_vardecl(payload, &funcscope);
        funcdecl.args.push_back(vardecl);
    }

    // skip the close parenthesis
    cursor.skip();

    // next token should be ":" for the return type
    if (!cursor.is_type(Token::Type::t_colon)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_colon, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    // skip the colon
    cursor.skip();

    // parse the return type
    if (!can_parse_type(payload)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    funcdecl.return_type = &parse_type(payload);

    // if next token is a semicolon we are done for now
    if (cursor.is_type(Token::Type::t_semicolon)) {
        cursor.skip();
        return;
    }

    // if the next token is an open brace, we parse the function body
    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_brace, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    // skip the open brace
    cursor.skip();

    // push the function scope
    payload.context.push_scope(funcscope);

    funcdecl.body = &parse_scope(payload);

    // pop the function scope
    payload.context.pop_scope();

    payload.context.scope().children.push_back(AST::make_ref(funcdecl));
}